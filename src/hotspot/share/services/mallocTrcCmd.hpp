#ifndef SHARE_SERVICES_STUEFEDCMD_HPP
#define SHARE_SERVICES_STUEFEDCMD_HPP

#include "services/diagnosticCommand.hpp"
#include "utilities/debug.hpp"

class MallocTrcCmd : public DCmdWithParser {
protected:
  DCmdArgument<char*> _what;
public:
  MallocTrcCmd(outputStream* output, bool heap);
  static const char* name() { return "VM.malloctrc"; }
  static const char* description() {
    return "bla bla bla.";
  }
  static const char* impact() { return "Low"; }
  virtual void execute(DCmdSource source, TRAPS);
};

#endif // SHARE_SERVICES_STUEFEDCMD_HPP
