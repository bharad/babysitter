/**
 * Worker bee
 * This class is responsible for finding the dependencies
 * required by a binary and building the full path
 * for the dependencies
 **/
/** includes **/
#include <stdio.h>
#include <string.h>
#include <error.h>
#include <string>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <gelf.h>
#include <pwd.h>
#include <set>
#include <regex.h>

/** defines **/
#define ERR -1
#define DEFAULT_PATH "/bin:/usr/bin:/usr/local/bin:/sbin;"

/** types **/
typedef std::set<std::string> string_set;

class WorkerBee {
private:
  Elf32_Ehdr *elf_header;	  	// Elf header object reference
  Elf *elf;                   // Elf reference
  Elf_Scn *scn;               // Seciton Descriptor
  Elf_Data *edata;            // Data description
  GElf_Sym sym;               // Symbols
  GElf_Shdr shdr;             // Section header
  
public:
  WorkerBee() {}
  ~WorkerBee() {}

  string_set *libs_for(const std::string &str);
  bool build_chroot(std::string &path, string_set &executables, string_set &extra_dirs);

// Functions
private:
  bool matches_pattern(const std::string & matchee, const char * pattern, int flags);
  bool is_lib(const std::string &n);
  std::pair<string_set *, string_set *> *linked_libraries(const std::string str);
  /** Utilities **/
  int make_path(const std::string & path);
  int cp_r(const std::string &source, const std::string &dest);
  int cp(const std::string & source, const std::string & destination);
  std::string find_binary(const std::string& file);
  bool abs_path(const std::string & path);
};
