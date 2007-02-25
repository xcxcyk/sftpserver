#include "sftpclient.h"
#include "utils.h"
#include "xfns.h"
#include "send.h"
#include "globals.h"
#include "types.h"
#include "alloc.h"
#include "parse.h"
#include "sftp.h"
#include "debug.h"
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <stdio.h>

static int sftpin;
static struct allocator allocator;
static struct sftpjob fakejob;
static struct worker fakeworker;
static char *cwd;
const struct sftpprotocol *protocol = &sftpv3;
const char sendtype[] = "request";

/* Command line */
static size_t buffersize = 32768;
static int nrequests = 8;
static const char *subsystem = "sftp";
static const char *program;
static const char *batchfile;
static int sshversion;
static int compress;
static const char *sshconf;
static const char *sshoptions[1024];
static int nsshoptions;
static int sshverbose;
static int sftpversion = 3;

static const struct option options[] = {
  { "help", no_argument, 0, 'h' },
  { "version", no_argument, 0, 'V' },
  { "buffer", required_argument, 0, 'B' },
  { "batch", required_argument, 0, 'b' },
  { "program", required_argument, 0, 'P' },
  { "requests", required_argument, 0, 'R' },
  { "subsystem", required_argument, 0, 's' },
  { "sftp-version", required_argument, 0, 'S' },
  { "1", no_argument, 0, '1' },
  { "2", no_argument, 0, '2' },
  { "C", no_argument, 0, 'C' },
  { "F", required_argument, 0, 'F' },
  { "o", required_argument, 0, 'o' },
  { "v", no_argument, 0, 'v' },
  { 0, 0, 0, 0 }
};

/* display usage message and terminate */
static void help(void) {
  xprintf("Usage:\n"
	 "  sftpclient [OPTIONS] [USER@]HOST\n"
	 "\n"
	 "Quick and dirty SFTP client\n"
	 "\n"
	 "Options:\n"
	 "  --help, -h               Display usage message\n"
	 "  --version, -V            Display version number\n"
	 "  -B, --buffer BYTES       Select buffer size (default 32768)\n"
	 "  -b, --batch PATH         Read batch file\n"
	 "  -P, --program PATH       Execute program as SFTP server\n"
	 "  -R, --requests COUNT     Maximum outstanding requests (default 8)\n"
	 "  -s, --subsystem NAME     Remote subsystem name\n"
	 "  -S, --sftp-version VER   Protocol version to request (default 3)\n"
	 "Options passed to SSH:\n"
	 "  -1, -2                   Select protocol version\n"
	 "  -C                       Enable compression\n"
	 "  -F PATH                  Use alternative  config file\n"
	 "  -o OPTION                Pass option to client\n"
	 "  -v                       Raise logging level\n");
  exit(0);
}

/* display version number and terminate */
static void version(void) {
  xprintf("sftp client version %s\n", VERSION);
  exit(0);
}

/* Get a response.  Picks out the type and ID. */
static uint8_t getresponse(int expected, uint32_t expected_id) {
  uint32_t len;
  uint8_t type;

  if(do_read(sftpin, &len, sizeof len))
    fatal("unexpected EOF from server while reading length");
  free(fakejob.data);                   /* free last job */
  fakejob.len = ntohl(len);
  fakejob.data = xmalloc(len);
  if(do_read(sftpin, fakejob.data, fakejob.len))
    fatal("unexpected EOF from server while reading data");
  if(DEBUG) {
    D(("response:"));
    hexdump(fakejob.data, fakejob.len);
  }
  fakejob.left = fakejob.len;
  fakejob.ptr = fakejob.data;
  cpcheck(parse_uint8(&fakejob, &type));
  if(expected > 0 && type != expected)
    fatal("expected response %d got %d", expected, type);
  if(type != SSH_FXP_VERSION) {
    cpcheck(parse_uint32(&fakejob, &fakejob.id));
    if(expected_id && fakejob.id != expected_id)
      fatal("wrong ID in response (want %"PRIu32" got %"PRIu32,
            expected_id, fakejob.id);
  }
  return type;
}

/* Split a command line */
static int split(const char *path, int lineno, char *line, char **av) {
  char *arg;
  int ac = 0;

  while(*line) {
    if(isspace((unsigned char)*line)) {
      ++line;
      continue;
    }
    if(*line == '"') {
      arg = av[ac++] = line;
      while(*line && *line != '"') {
	if(*line == '\\' && line[1])
	  ++line;
	*arg++ = *line++;
      }
      if(!*line) {
	fprintf(stderr, "%s:%d: unterminated string\n", path, lineno);
	return -1;
      }
      *arg++ = 0;
      line++;
    } else {
      av[ac++] = line;
      while(*line && !isspace((unsigned char)*line))
	++line;
      if(*line)
	*line++ = 0;
    }
  }
  av[ac] = 0;
  return ac;
}

static void status(const char *path,
                   int lineno) {
  uint32_t status;
  char *msg;
  
  cpcheck(parse_uint32(&fakejob, &status));
  cpcheck(parse_string(&fakejob, &msg, 0));
  fprintf(stderr, "%s:%d: %s (%s)\n", path, lineno,
          msg, status_to_string(status));
}

static int cmd_pwd(const char attribute((unused)) *path,
                   int attribute((unused)) lineno, 
                   int attribute((unused)) ac,
                   char attribute((unused)) **av) {
  xprintf("%s\n", cwd);
  return 0;
}

static int cmd_cd(const char attribute((unused)) *path,
                   int attribute((unused)) lineno, 
                   int attribute((unused)) ac,
                   char **av) {
  char *newcwd;
  uint32_t u32;

  /* Send SSH_FXP_REALPATH . to find path to current directory */
  send_begin(&fakejob);
  send_uint8(&fakejob, SSH_FXP_REALPATH);
  send_uint32(&fakejob, 1);
  send_path(&fakejob, av[0]);
  send_end(&fakejob);
  switch(getresponse(-1, 1)) {
  case SSH_FXP_NAME:
    cpcheck(parse_uint32(&fakejob, &u32));
    if(u32 != 1) fatal("wrong count in REALPATH reply");
    cpcheck(parse_path(&fakejob, &newcwd));
    free(cwd);
    cwd = xstrdup(newcwd);
    break;
  case SSH_FXP_STATUS:
    status(path, lineno);
    return -1;
    break;
  }
  return 0;
}
static const struct {
  const char *name;
  int minargs, maxargs;
  int (*handler)(const char *path, int lineno, int ac, char **av);
} commands[] = {
  { "cd", 1, 1, cmd_cd },
  { "pwd", 0, 0, cmd_pwd },
  { 0, 0, 0, 0 }
};

static void process(const char *prompt, const char *path, FILE *fp) {
  int lineno = 0;
  char buffer[4096];
  int ac, n;
  char *avbuf[256], **av;
 
  if(prompt) {
    fputs(prompt, stdout);
    fflush(stdout);
  }
  while(fgets(buffer, sizeof buffer, fp)) {
    ++lineno;
    if((ac = split(path, lineno, buffer, av = avbuf)) < 0 && !prompt)
      exit(1);
    if(!ac) goto next;
    for(n = 0; commands[n].name && strcmp(av[0], commands[n].name); ++n)
      ;
    if(!commands[n].name) {
      fprintf(stderr, "%s: %d: unknown command: '%s'\n", path, lineno, av[0]);
      if(!prompt) exit(1);
      goto next;
    }
    ++av;
    --ac;
    if(ac < commands[n].minargs || ac > commands[n].maxargs) {
      fprintf(stderr, "%s:%d: wrong number of arguments\n", path, lineno);
      if(!prompt) exit(1);
      goto next;
    }
    if(commands[n].handler(path, lineno, ac, av) && !prompt)
      exit(1);
next:
    alloc_destroy(fakejob.a);
    if(prompt) {
      fputs(prompt, stdout);
      fflush(stdout);
    }
  }
  if(ferror(fp))
    fatal("error reading %s: %s", path, strerror(errno));
}

int main(int argc, char **argv) {
  const char *cmdline[2048];
  int n, ncmdline;
  int ip[2], op[2];
  pid_t pid;
  uint32_t u32;
  
  while((n = getopt_long(argc, argv, "hVB:b:P:R:s:S:12CF:o:v",
			 options, 0)) >= 0) {
    switch(n) {
    case 'h': help();
    case 'V': version();
    case 'B': buffersize = atoi(optarg); break;
    case 'b': batchfile = optarg; break;
    case 'P': program = optarg; break;
    case 'R': nrequests = atoi(optarg); break;
    case 's': subsystem = optarg;
    case 'S': sftpversion = atoi(optarg); break;
    case '1': sshversion = 1; break;
    case '2': sshversion = 2; break;
    case 'C': compress = 1; break;
    case 'F': sshconf = optarg; break;
    case 'o': sshoptions[nsshoptions++] = optarg; break;
    case 'v': sshverbose++; break;
    default: exit(1);
    }
  }

  if(sftpversion != 3)
    fatal("unknown SFTP version %d", sftpversion);
  
  ncmdline = 0;
  if(program) {
    cmdline[ncmdline++] = program;
  } else {
    cmdline[ncmdline++] = "ssh";
    if(optind >= argc)
      fatal("missing USER@HOST argument");
    if(sshversion == 1) cmdline[ncmdline++] = "-1";
    if(sshversion == 2) cmdline[ncmdline++] = "-2";
    if(compress) cmdline[ncmdline++] = "-C";
    if(sshconf) {
      cmdline[ncmdline++] = "-F";
      cmdline[ncmdline++] = sshconf;
    }
    for(n = 0; n < nsshoptions; ++n) {
      cmdline[ncmdline++] = "-o";
      cmdline[ncmdline++] = sshoptions[n++];
    }
    while(sshverbose-- > 0)
      cmdline[ncmdline++] = "-v";
    cmdline[ncmdline++] = "-s";
    cmdline[ncmdline++] = argv[optind++];
    cmdline[ncmdline++] = subsystem;
  }
  cmdline[ncmdline] = 0;
  for(n = 0; n < ncmdline; ++n)
    fprintf(stderr, " %s", cmdline[n]);
  fputc('\n',  stderr);
  xpipe(ip);
  xpipe(op);
  if(!(pid = xfork())) {
    xclose(ip[0]);
    xclose(op[1]);
    xdup2(ip[1], 1);
    xdup2(op[0], 0);
    execvp(cmdline[0], (void *)cmdline);
    fatal("executing %s: %s", cmdline[0], strerror(errno));
  }
  xclose(ip[1]);
  xclose(op[0]);
  sftpin = ip[0];
  sftpout = op[1];
  fakejob.a = alloc_init(&allocator);
  fakejob.worker = &fakeworker;

  /* Send SSH_FXP_INIT */
  send_begin(&fakejob);
  send_uint8(&fakejob, SSH_FXP_INIT);
  send_uint32(&fakejob, sftpversion);
  send_end(&fakejob);
  
  /* Parse the version reponse */
  getresponse(SSH_FXP_VERSION, 0);
  cpcheck(parse_uint32(&fakejob, &u32));
  if(u32 != 3)
    fatal("we only know protocol version 3 but server wanted version %"PRIu32,
          u32);
  /* TODO parse extensions */

  /* Send SSH_FXP_REALPATH . to find path to current directory */
  send_begin(&fakejob);
  send_uint8(&fakejob, SSH_FXP_REALPATH);
  send_uint32(&fakejob, 1);
  send_path(&fakejob, ".");
  send_end(&fakejob);
  getresponse(SSH_FXP_NAME, 1);
  cpcheck(parse_uint32(&fakejob, &u32));
  if(u32 != 1) fatal("wrong count in REALPATH reply");
  cpcheck(parse_path(&fakejob, &cwd));
  cwd = xstrdup(cwd);

  if(batchfile) {
    FILE *fp;

    if(!(fp = fopen(batchfile, "r")))
      fatal("error opening %s: %s", batchfile, strerror(errno));
    process(0, batchfile, fp);
  } else
    process("sftp> ", "stdin", stdin);
  /* We let the OS reap the SSH process */
  return 0;
}

/*
Local Variables:
c-basic-offset:2
comment-column:40
fill-column:79
indent-tabs-mode:nil
End:
*/