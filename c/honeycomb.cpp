/*---------------------------- Includes ------------------------------------*/
#include <string>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <assert.h>
#include <fcntl.h>
#include <gelf.h>
#include <regex.h>
#include <libgen.h>
// System
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>

// Include sys/capability if the system can handle it. 
#ifdef HAVE_SYS_CAPABILITY_H
#include <sys/capability.h>
#endif
// Erlang interface
#include "ei++.h"

#include "honeycomb_config.h"
#include "honeycomb.h"
#include "worker_bee.h"
#include "hc_support.h"

/*---------------------------- Implementation ------------------------------*/

using namespace ei;

int Honeycomb::setup_defaults() {
  /* Setup environment defaults */
  const char* default_env_vars[] = {
   "LD_LIBRARY_PATH=/lib;/usr/lib;/usr/local/lib", 
   "HOME=/mnt"
  };
  
  int m_cenv_c = 0;
  const int max_env_vars = 1000;
  
  if ((m_cenv = (const char**) new char* [max_env_vars]) == NULL) {
    m_err << "Could not allocate enough memory to create list"; return -1;
  }
  
  memcpy(m_cenv, default_env_vars, (std::min((int)max_env_vars, (int)sizeof(default_env_vars)) * sizeof(char *)));
  m_cenv_c = sizeof(default_env_vars) / sizeof(char *);
  
  return 0;
}

/**
 * Decode the erlang tuple
 * The erlang tuple decoding happens for all the tuples that get sent over the wire
 * to the c-node.
 **/
int Honeycomb::ei_decode(ei::Serializer& ei) {
  // {Cmd::string(), [Option]}
  //      Option = {env, Strings} | {cd, Dir} | {kill, Cmd}
  int sz;
  std::string op_str, val;
  
  m_err.str("");
  delete [] m_cenv;
  m_cenv = NULL;
  m_env.clear();
  m_nice = INT_MAX;
    
  if (ei.decodeString(m_cmd) < 0) {
    m_err << "badarg: cmd string expected or string size too large" << m_cmd << "Command!";
    return -1;
  } else if ((sz = ei.decodeListSize()) < 0) {
    m_err << "option list expected";
    return -1;
  } else if (sz == 0) {
    m_cd  = "";
    m_kill_cmd = "";
    return 0;
  }
  
  // Run through the commands and decode them
  enum OptionT            { CD,   ENV,   KILL,   NICE,   USER,   STDOUT,   STDERR,  MOUNT } opt;
  const char* options[] = {"cd", "env", "kill", "nice", "user", "stdout", "stderr", "mount"};
  
  for(int i=0; i < sz; i++) {
    if (ei.decodeTupleSize() != 2 || (int)(opt = (OptionT)ei.decodeAtomIndex(options, op_str)) < 0) {
      m_err << "badarg: cmd option must be an atom"; 
      return -1;
    }
    DEBUG_MSG("Found option: %s\n", op_str.c_str());
    switch(opt) {
      case CD:
      case KILL:
      case USER: {
        // {cd, Dir::string()} | {kill, Cmd::string()} | {user, Cmd::string()} | etc.
        if (ei.decodeString(val) < 0) {m_err << opt << " bad option"; return -1;}
        if (opt == CD) {m_cd = val;}
        else if (opt == KILL) {m_kill_cmd = val;}
        else if (opt == USER) {
          struct passwd *pw = getpwnam(val.c_str());
          if (pw == NULL) {m_err << "Invalid user: " << val << " : " << ::strerror(errno); return -1;}
          m_user = pw->pw_uid;
        }
        break;
      }
      case NICE: {
        if (ei.decodeInt(m_nice) < 0 || m_nice < -20 || m_nice > 20) {m_err << "Nice must be between -20 and 20"; return -1;}
        break;
      }
      case ENV: {
        int env_sz = ei.decodeListSize();
        if (env_sz < 0) {m_err << "Must pass a list for env option"; return -1;}
        
        for(int i=0; i < env_sz; i++) {
          std::string str;
          if (ei.decodeString(str) >= 0) {
            m_env.push_back(str);
            m_cenv[m_cenv_c+i] = m_env.back().c_str();
          } else {m_err << "Invalid env argument at " << i; return -1;}
        }
        m_cenv[m_cenv_c+1] = NULL; // Make sure we have a NULL terminated list
        m_cenv_c = env_sz+m_cenv_c; // save the new value... we don't really need to do this, though
        break;
      }
      case STDOUT:
      case STDERR: {
        int t = 0;
        int sz = 0;
        std::string str, fop;
        t = ei.decodeType(sz);
        if (t == ERL_ATOM_EXT) ei.decodeAtom(str);
        else if (t == ERL_STRING_EXT) ei.decodeString(str);
        else {
          m_err << "Atom or string tuple required for " << op_str;
          return -1;
        }
        // Setup the writer
        std::string& rs = (opt == STDOUT) ? m_stdout : m_stderr;
        std::stringstream stream;
        int fd = (opt == STDOUT) ? 1 : 2;
        if (str == "null") {stream << fd << ">/dev/null"; rs = stream.str();}
        else if (str == "stderr" && opt == STDOUT) {rs = "1>&2";}
        else if (str == "stdout" && opt == STDERR) {rs = "2>&1";}
        else if (str != "") {stream << fd << ">\"" << str << "\"";rs = stream.str();}
        break;
      }
      default:
        m_err << "bad options: " << op_str; return -1;
    }
  }
  
  if (m_stdout == "1>&2" && m_stderr != "2>&1") {
    m_err << "cirtular reference of stdout and stderr";
    return -1;
  } else if (!m_stdout.empty() || !m_stderr.empty()) {
    std::stringstream stream; stream << m_cmd;
    if (!m_stdout.empty()) stream << " " << m_stdout;
    if (!m_stderr.empty()) stream << " " << m_stderr;
    m_cmd = stream.str();
  }
  
  return 0;
}

#ifndef MAX_ARGS
#define MAX_ARGS 64
#endif
// Run a hook on the system
int Honeycomb::comb_exec(std::string cmd) {
  setup_defaults(); // Setup default environments
  const std::string shell = getenv("SHELL");  
  const std::string shell_args = "-c";
  const char* argv[] = { shell.c_str(), shell_args.c_str(), cmd.c_str(), NULL };
  pid_t pid; // Pid of the child process
  int status;
  
  // If we are looking at shell script text
  if ( strncmp(cmd.c_str(), "#!", 2) == 0 ) {
    char filename[40];
    int size, fd;
    
    snprintf(filename, 40, "/tmp/babysitter.XXXXXXXXX");
    
    // Make a tempfile in the filename format
    if ((fd = mkstemp(filename)) == -1) {
      fprintf(stderr, "Could not open tempfile: %s\n", filename);
      return -1;
    }
    
    size = strlen(cmd.c_str());
    // Print the command into the file
    if (write(fd, cmd.c_str(), size) == -1) {
      fprintf(stderr, "Could not write command to tempfile: %s\n", filename);
      return -1;
    }
    
    // Confirm that the command is written
    if (fsync(fd) == -1) {
      fprintf(stderr, "fsync failed for tempfile: %s\n", filename);
      return -1;
    }
    
    close(fd);
    
    // Modify the command to match call the filename
    std::string sFile (filename);
    
		if (chown(sFile.c_str(), m_user, m_group) != 0) {
#ifdef DEBUG
     fprintf(stderr, "Could not change owner of '%s' to %d\n", sFile.c_str(), m_user);
#endif
		}

    // Make it executable
    if (chmod(sFile.c_str(), 040750) != 0) {
      fprintf(stderr, "Could not change permissions to '%s' %o\n", sFile.c_str(), 040700);
    }
    
    // Run in a new process
    if ((pid = fork()) == -1) {
      perror("fork");
      return -1;
    }
    if (pid == 0) {
      // we are in a new process
      argv[2] = sFile.c_str();
      argv[3] = NULL;
      
      if (execve(argv[0], (char* const*)argv, (char* const*)m_cenv) < 0) {
        fprintf(stderr, "Cannot execute '%s' because '%s'", cmd.c_str(), ::strerror(errno));
        perror("execute");
        unlink(filename);
        return -1;
      }
    }
    while (wait(&status) != pid) ; // We don't want to continue until the child process has ended
    assert(0 == status);
    // Cleanup :)
    printf("Cleaning up...\n");
    unlink(filename);
  } else {
    
    // First, we have to construct the command
    int argc = 0;
    char *str_cmd = strdup(cmd.c_str());
    argv[argc] = strtok(str_cmd, " \r\t\n");
    
    while (argc++ < MAX_ARGS) if (! (argv[argc] = strtok(NULL, " \t\n")) ) break;
    
    // Run in a new process
    if ((pid = fork()) == -1) {
      perror("fork");
      return -1;
    }
    if (pid == 0) {
      // we are in the child process
      if (execve(argv[0], (char* const*)argv, (char* const*)m_cenv) < 0) {
        fprintf(stderr, "Cannot execute '%s' because '%s'\n", argv[0], ::strerror(errno));
        return EXIT_FAILURE;
      }
    }
    
    while (wait(&status) != pid) ; // We don't want to continue until the child process has ended
    assert(0 == status);
  }
  
  return 0;
}

// Execute a hook
void Honeycomb::exec_hook(std::string action, int stage, phase *p) {
  if (stage == BEFORE) {
    if (p->before) comb_exec(p->before); //printf("Run before hook for %s: %s\n", action.c_str(), p->before);
  } else if (stage == AFTER) {
    if (p->after) comb_exec(p->after); //printf("Run after hook for %s %s\n", action.c_str(), p->after);
  } else {
    printf("Unknown hook for: %s %d\n", action.c_str(), stage);
  }
}

void Honeycomb::ensure_cd_exists() {
  struct stat stt;
  if (0 == stat(m_root_dir.c_str(), &stt)) {
    // Should we check on the ownership?
  } else if (ENOENT == errno) {
    if (mkdir(m_root_dir.c_str(), m_mode)) {
      fprintf(stderr, "Error: %s and could not create the directory: %s\n", ::strerror(errno), m_root_dir.c_str());
    }
  } else {
    fprintf(stderr, "Unknown error: %s Exiting...\n", ::strerror(errno));
    exit(-1);
  }
  
  // Not sure if this and the next step is appropriate here anymore... *thinking about it*
  if (mkdir(m_cd.c_str(), m_mode)) {
    m_err << "Could not create the new confinement root " << m_cd;
  }
  
  // Make the directory owned by the user
  if (chown(m_cd.c_str(), m_user, m_group)) {
    m_err << "Could not chown to the effective user: " << m_user;
  }
}

//---
// ACTIONS
//---

int Honeycomb::bundle() {
  phase *p = find_phase(m_honeycomb_config, T_BUNDLE);
  temp_drop();
  exec_hook("bundle", BEFORE, p);
  // Run command
  //--- Make sure the directory exists
  ensure_cd_exists();
  if ((p != NULL) && (p->command != NULL)) {
    printf("Running client code instead\n");
    printf("p: %s\n", p->command);
  } else {
    //Default action
    printf("Running default action for bundle\n");
    WorkerBee b;

    b.build_chroot(m_cd, m_user, m_group, m_executables, m_files, m_dirs);
  }
  
  exec_hook("bundle", AFTER, p);
  
  // Set our resource limits (TODO: Move to mounting?)
  set_rlimits();
  // come back to our permissions
  restore_perms();
  return 0;
}

pid_t Honeycomb::execute() {
  pid_t chld = fork();
  
  // We are in the child pid
  perm_drop(); // Drop into new user forever!!!!
  
  if(chld < 0) {
    fprintf(stderr, "Could not fork into new process :(\n");
    return(-1);
   } else { 
    // Build the environment vars
    const std::string shell = getenv("SHELL");
    const std::string shell_args = "-c";
    const char* argv[] = { shell.c_str(), shell_args.c_str(), m_cmd.c_str() };
   
    if (execve(m_cmd.c_str(), (char* const*)argv, (char* const*)m_cenv) < 0) {
      fprintf(stderr, "Cannot execute '%s' because '%s'", m_cmd.c_str(), ::strerror(errno));
      return EXIT_FAILURE;
    }
  } 
  
  if (m_nice != INT_MAX && setpriority(PRIO_PROCESS, chld, m_nice) < 0) {
    fprintf(stderr, "Cannot set priority of pid %d", chld);
    return(-1);
  }
  
  return chld;
}

void Honeycomb::set_rlimits() {
  if(m_nofiles) set_rlimit(RLIMIT_NOFILE, m_nofiles);
}

int Honeycomb::set_rlimit(const int res, const rlim_t limit) {
  struct rlimit lmt = { limit, limit };
  if (setrlimit(res, &lmt)) {
    fprintf(stderr, "Could not set resource limit: %d\n", res);
    return(-1);
  }
  return 0;
}

/*---------------------------- UTILS ------------------------------------*/

const char *DEV_RANDOM = "/dev/urandom";
uid_t Honeycomb::random_uid() {
  uid_t u;
  for (unsigned char i = 0; i < 10; i++) {
    int rndm = open(DEV_RANDOM, O_RDONLY);
    if (sizeof(u) != read(rndm, reinterpret_cast<char *>(&u), sizeof(u))) {
      continue;
    }
    close(rndm);

    if (u > 0xFFFF) {
      return u;
    }
  }

  DEBUG_MSG("Could not generate a good random UID after 10 attempts. Bummer!");
  return(-1);
}
const char * const Honeycomb::to_string(long long int n, unsigned char base) {
  static char bfr[32];
  const char * frmt = "%lld";
  if (16 == base) {frmt = "%llx";} else if (8 == base) {frmt = "%llo";}
  snprintf(bfr, sizeof(bfr) / sizeof(bfr[0]), frmt, n);
  return bfr;
}

/*---------------------------- Permissions ------------------------------------*/

int Honeycomb::temp_drop() {
  DEBUG_MSG("Dropping into '%d' user\n", config->user);
  if (setresgid(-1, m_user, getegid()) || setresuid(-1, m_user, geteuid()) || getegid() != m_user || geteuid() != m_user ) {
    fprintf(stderr, "Could not drop privileges temporarily to %d: %s\n", m_user, ::strerror(errno));
    return -1; // we are in the fork
  }
#ifdef DEBUG
  printf("Dropped into user %d\n", m_user);
#endif
  return 0;
}
int Honeycomb::perm_drop() {
  uid_t ruid, euid, suid;
  gid_t rgid, egid, sgid;

  if (setresgid(m_user, m_user, m_user) || setresuid(m_user, m_user, m_user) || getresgid(&rgid, &egid, &sgid)
      || getresuid(&ruid, &euid, &suid) || rgid != m_user || egid != m_user || sgid != m_user
      || ruid != m_user || euid != m_user || suid != m_user || getegid() != m_user || geteuid() != m_user ) {
    fprintf(stderr,"\nError setting user to %u\n",m_user);
    return -1;
  }
  return 0;
}
int Honeycomb::restore_perms() {
  uid_t ruid, euid, suid;
  gid_t rgid, egid, sgid;

  if (getresgid(&rgid, &egid, &sgid) || getresuid(&ruid, &euid, &suid) || setresuid(-1, suid, -1) || setresgid(-1, sgid, -1)
      || geteuid() != suid || getegid() != sgid ) {
        fprintf(stderr, "Could not drop privileges temporarily to %d: %s\n", m_user, ::strerror(errno));
        return -1; // we are in the fork
      }
  return 0;
}

/*------------------------ INTERNAL -------------------------*/
void Honeycomb::init() {
  ei::Serializer m_eis(2);
  m_nofiles = NULL;
  std::stringstream stream;
  std::string token;
  
  // Setup defaults here
  m_user = INT_MAX;
  m_group = INT_MAX;
  
  // Run through the config
  if (m_honeycomb_config != NULL) {
    //--- User
    if (m_honeycomb_config->user != NULL) {
      struct passwd *pw = getpwnam(m_honeycomb_config->user);
      if (pw == NULL) {
        fprintf(stderr, "Error: invalid user %s : %s\n", m_honeycomb_config->user, ::strerror(errno));
        m_user = geteuid();
      } else m_user = pw->pw_uid;
    } else m_user = random_uid();
    
    //--- group
    if (m_honeycomb_config->group != NULL) {
      struct group *grp = getgrnam(m_honeycomb_config->group);
      if (grp == NULL) {
        fprintf(stderr, "Error: invalid group %s : %s\n", m_honeycomb_config->group, ::strerror(errno));
        m_group = getgid();
      }
      else m_group = grp->gr_gid;
    } else m_group = getgid();
    
    //--- root_directory
    m_root_dir = "/var/beehive/honeycombs"; // Default
    if (m_honeycomb_config->root_dir != NULL) {
      m_root_dir = m_honeycomb_config->root_dir;
    }
    
    //--- executables
    m_executables.insert("/bin/ls");
    m_executables.insert("/bin/bash");
    m_executables.insert("/usr/bin/whoami");
    m_executables.insert("/usr/bin/env");
    
    if (m_honeycomb_config->executables != NULL) {
      stream << m_honeycomb_config->executables; // Insert into a string
      while ( getline(stream, token, ' ') ) m_executables.insert(token);
    }
    
    //--- directories
    if (m_honeycomb_config->directories != NULL) {
      stream.clear(); token = "";
      stream << m_honeycomb_config->directories;
      while ( getline(stream, token, ' ') ) m_dirs.insert(token);
    }
    
    //--- other files
    if (m_honeycomb_config->files != NULL) {
      stream.clear(); token = "";
      stream << m_honeycomb_config->files;
      while ( getline(stream, token, ' ') ) m_files.insert(token);
    }
    
    //--- run directory
    if (m_honeycomb_config->run_dir != NULL) m_run_dir = m_honeycomb_config->run_dir;
    
    //--- image
    if (m_honeycomb_config->image != NULL) m_image = m_honeycomb_config->image;
    //--- skel directory
    if (m_honeycomb_config->skel_dir != NULL) m_image = m_honeycomb_config->skel_dir;    
    
    //--- confinement_mode
    m_mode = 04755; // Not sure if this should be dynamic-able, yet.
    
    // The m_cd path is the confinement_root plus the user's uid
    // because it's randomly generated
    m_cd = m_root_dir + "/" + to_string(m_user, 10);    
  }
}

int Honeycomb::valid() {
  // Run validations on the honeycomb here
  return 0;
}