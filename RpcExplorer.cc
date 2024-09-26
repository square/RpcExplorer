/*
 * Copyright 2023 Block Inc.
 */

#include <getopt.h>
#include <strings.h>
#include <wordexp.h>
#include <signal.h>
#include <iostream>
#include <sstream>
#include <fstream>
#include <regex>
#include <string>
#include <filesystem>
#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/stubs/strutil.h>
#include <cdk.h>
#include <cdk/cdk_objs.h>
#include <unordered_set>
#include <unordered_map>

using namespace google::protobuf::compiler;
using namespace google::protobuf::util;
using google::protobuf::FileDescriptor;
using google::protobuf::Reflection;
using google::protobuf::ServiceDescriptor;
using google::protobuf::MethodDescriptor;
using google::protobuf::EnumDescriptor;
using google::protobuf::EnumValueDescriptor;
using google::protobuf::FieldDescriptor;
using google::protobuf::Descriptor;
using google::protobuf::DynamicMessageFactory;
using google::protobuf::Message;
using google::protobuf::Base64Unescape;
using google::protobuf::Base64Escape;
using google::protobuf::DebugStringOptions;

std::string tolower(std::string input);
std::string getInput(CDKSCREEN* cdk_screen, const char* title, const char* label);
static void unsetFocus(CDKOBJS* obj);
int num_rows, num_cols;
class RequestBuilderPage;

#define ROWS_FOR_ONSCREEN_HELP 8

// Mostly for convenience, although this is technically bad practice.
static DynamicMessageFactory dynamic_message_factory;

/**
 * Command line options for this program.
 */
struct Options {
  /**
   * If verbose > 0, print debugging information to stderr.
   */
  int verbose;

  /**
   * A list of diretories to look for proto files in when searching for services.
   * If omitted, the current working directory is assumed.
   */
  std::vector<const char*> protoPaths;

  /**
   * A list of root proto files to search over, as relative paths from the
   * protoPaths. These files and their dependencies will be searched over. If
   * not provided, all proto files in the protoPaths will be searched.
   */
  std::vector<const char*> protoFiles;

  /**
   * The name of the file containing the request template that
   * RpcExplorer will inject fields into. If not provided, will be
   * requested on demand.
   */
  std::string request_template;
};

enum FieldCdkType {ENTRY, EXPAND_BUTTON, ADD_BUTTON, REQUEST_BUTTON, EXPORT_BUTTON};
/**
 * An POD that stores information about a CDK object that is used for
 * collecting user input about a proto field.
 */
struct ProtoCDKField {
  /**
   * This points to a Button if the field is a Message type or an Entry
   * otherwise.
   */
  CDKOBJS* field_cdk_obj;

  /**
   * The type of field_cdk_obj.
   */
  enum FieldCdkType field_cdk_type;

  /**
   * The contents of the entry when field_cdk_obj is an ENTRY, used to cache
   * the value across destruction and reconstruction.
   */
  char* field_cached_value;

  /**
   * The memory for the CDK label.
   */
  CDKLABEL* field_cdk_label;

  /**
   * The row of the virtual screen that this object should be rendered on.
   */
  int field_virtual_row;

  /**
   * The memory for the field label string.
   */
  char* field_label_string;

  /**
   * The descriptor for the proto field that the user input should correspond to.
   */
  const FieldDescriptor* field_descriptor;

  /**
   * The ProtoCDKField that that have been added as a result of this field.
   * When field_descriptor->is_repeated() is true, and the user hits Add, this
   * contains the child fields.
   * When field_descriptor->type() is TYPE_MESSAGE, and the user hits expand,
   * this contains the proto fields of the child message.
   */
  std::vector<ProtoCDKField*> children;

  /**
   * Used for indentation during output.
   */
  int tab_index;

  /**
   * This field only applies when the FieldDescriptor corresponds to a Messsage type.
   * True means the expand button has already been hit, which means both of the following behaviors are in force:
   * 1. We no longer render an Expand button.
   * 2. We skip over this ProtoCDKField when iterating the list of fields.
   */
  int hide_expand;
};

/**
 * Used for printing debug messages to a file, for convenient separation from
 * the main curses UI.
 */
static int debugMsg(const char *fmt, ...)
{
  static const char* path = getenv("DEBUG_FILE");
  if (path) {
    FILE* file = fopen(path, "a");
    char buffer[4096];
    va_list args;
    va_start(args, fmt);
    int rc = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    rc = fputs(buffer, file);
    fclose(file);
    return rc;
  }
  return 0;
}

/**
 * Print usage to stderr and exit.
 */
static void usage() {
  std::cerr <<
       "Usage: RpcExplorer [--proto_path=PATH...] [--request_template TEMPLATE] [--verbose] [proto_file...]  \n"
       "\n"
       "  Arguments:\n"
       "    -IPATH, --proto_path=PATH   Specify the directory in which to search for\n"
       "                                protos.  May be specified multiple times;\n"
       "                                directories will be searched in order.  If not\n"
       "                                given, the current working directory is used.\n"
       "    --request_template TEMPLATE Specify the request template filename. This is a script-like file that contains two types of placeholders for substition:\n"
       "                                  1. Placeholders directly related to the service or proto that will be injected by the script based on normal user interaction. \n"
       "                                       ###{JSON_REQUEST}\n"
       "                                       ###{BASE64_PROTO_REQUEST}\n"
       "                                       ###{PROTO_DIRS}\n"
       "                                       ###{SERVICE_PROTO_FILE}\n"
       "                                       ###{REQUEST_PROTO_FILE}\n"
       "                                       ###{RESPONSE_PROTO_FILE}\n"
       "                                       ###{FULL_SERVICE_NAME}\n"
       "                                       ###{FULL_METHOD_NAME}\n"
       "                                       ###{SERVICE_NAME}\n"
       "                                       ###{METHOD_NAME}\n"
       "                                       ###{FULL_REQUEST_NAME}\n"
       "                                       ###{FULL_RESPONSE_NAME}\n"
       "                                  2. Placeholders to directly ask the user for. These can be any alphanumeric string and can contain spaces. The name will be displayed to the user.\n"
       "                                       ###{registry name}\n"
       "    proto_file                  When given, search only for methods and services in listed protos. This is an optimization.\n"
       "    --verbose                   When given, debug output will be printed to stderr.\n"
       "    --help                      Show this message.\n";
  exit(1);
}

/**
 * Attempt to parse command line arguments. Print out an informative message to
 * stderr if an error is encountered.
 */
Options parseArguments(int argc, char** argv) {
  // Initialize default options
  Options options;
  options.verbose = 0;

  static struct option long_options[] =
    {
      /* These options set a flag. */
      {"verbose", no_argument, &options.verbose, 1},
      {"help", no_argument, NULL, 'h'},
      {"proto_path", required_argument, 0, 'I'},
      {"request_template", required_argument, 0, 't'},
      {0, 0, 0, 0}
    };
  while (1) {
    int option_index = 0;
    int c;
    c = getopt_long(argc, argv, "hI:", long_options, &option_index);
    if (c == -1) {
      break;
    }
    switch (c) {
      // Do nothing if just set verbose flag
      case 0:
        break;
      case 'I':
        options.protoPaths.push_back(optarg);
        break;
      case 't':
        options.request_template = std::string(optarg);
        break;
      case 'h':
      default:
        usage();
    }
  }
  argc -= optind;
  argv += optind;

  // Make the protoPaths the current directory if empty.
  if (options.protoPaths.empty()) {
    options.protoPaths.push_back(".");
  }

  // Parse proto files to import if the user specified them.
  while (argc > 0) {
    options.protoFiles.push_back(argv[0]);
    argc--;
    argv++;
  }

  // Print options output to debug option parsing.
  if (options.verbose) {
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "\tverbose: %d\n", options.verbose);
    fprintf(stderr, "\trequest_template: %s\n", options.request_template.c_str());
    fprintf(stderr, "\tproto_paths:\n");
    for (int i = 0; i < options.protoPaths.size(); i++) {
      fprintf(stderr, "\t\t%s\n", options.protoPaths[i]);
    }
    fprintf(stderr, "\tproto_file:\n");
    for (int i = 0; i < options.protoFiles.size(); i++) {
      fprintf(stderr, "\t\t%s\n", options.protoFiles[i]);
    }
  }
  return options;
}

/**
 * Populate message body using reflection.
 */
void populateMessageData(Message* message, const std::vector<ProtoCDKField*> fields) {
  const Reflection* reflection = message->GetReflection();
  debugMsg("\tpopulateMessageData: Called with %d fields.\n", fields.size());
  for (int i = 0; i < fields.size(); i++) {
    const ProtoCDKField* proto_cdk_field = fields[i];
    const FieldDescriptor* field_descriptor = proto_cdk_field->field_descriptor;
    FieldDescriptor::Type field_type = field_descriptor->type();
    debugMsg("Found field proto_field %s with is_repeated %d\n",
        field_descriptor->full_name().c_str(),
        field_descriptor->is_repeated());
    // Handle repeated by iterating over children
    if (field_descriptor->is_repeated()) {
      for (ProtoCDKField* child : proto_cdk_field->children) {
        if (field_type == FieldDescriptor::Type::TYPE_MESSAGE) {
          if (child->hide_expand) {
            // We hit expand so we should populate recursively.
            populateMessageData(reflection->AddMessage(message,
                  field_descriptor), child->children);
          }
        } else {
          // Get the string associated with the field
          const char* value = ((CDKENTRY*)child->field_cdk_obj)->info;
          if (value == NULL || strlen(value) == 0) {
            continue;
          }
          // Handle based on the type
          switch(field_type) {
            case FieldDescriptor::Type::TYPE_DOUBLE:
              reflection->AddDouble(message, field_descriptor, atof(value));
              break;

            case FieldDescriptor::Type::TYPE_FLOAT:
              reflection->AddFloat(message, field_descriptor, atof(value));
              break;

            case FieldDescriptor::Type::TYPE_INT64:
            case FieldDescriptor::Type::TYPE_FIXED64:
            case FieldDescriptor::Type::TYPE_SFIXED64:
            case FieldDescriptor::Type::TYPE_SINT64:
              reflection->AddInt64(message, field_descriptor, atol(value));
              break;
            case FieldDescriptor::Type::TYPE_INT32:
            case FieldDescriptor::Type::TYPE_SFIXED32:
            case FieldDescriptor::Type::TYPE_FIXED32:
            case FieldDescriptor::Type::TYPE_SINT32:
              reflection->AddInt32(message, field_descriptor, atoi(value));
              break;

            case FieldDescriptor::Type::TYPE_UINT64:
              reflection->AddUInt64(message, field_descriptor, atol(value));
              break;
            case FieldDescriptor::Type::TYPE_UINT32:
              reflection->AddUInt32(message, field_descriptor, atoi(value));
              break;

            case FieldDescriptor::Type::TYPE_BOOL:
              {
                std::string lowercase = tolower(std::string(value));
                reflection->AddBool(message,
                    field_descriptor,
                    strcmp(lowercase.c_str(), "true") == 0 ||
                    value[0] == '1');
              }
              break;

            case FieldDescriptor::Type::TYPE_STRING:
              reflection->AddString(message, field_descriptor, std::string(value));
              break;
            case FieldDescriptor::Type::TYPE_BYTES:
              // Assume bytes are base 64 encoded
              {
                std::string binaryProto;
                if (Base64Unescape(value, &binaryProto)) {
                  reflection->AddString(message, field_descriptor, binaryProto);
                } else {
                  reflection->AddString(message, field_descriptor, value);
                }
                break;
              }

            case FieldDescriptor::Type::TYPE_ENUM:
              {
                const EnumDescriptor* enum_descriptor = field_descriptor->enum_type();
                const EnumValueDescriptor* enum_value_descriptor;
                if (isdigit(value[0])) {
                  enum_value_descriptor = enum_descriptor->FindValueByNumber(atoi(value));
                } else {
                  enum_value_descriptor = enum_descriptor->FindValueByName(value);
                }
                if (enum_value_descriptor != nullptr) {
                  reflection->AddEnum(message, field_descriptor, enum_value_descriptor);
                }
              }
              break;

            default:
              debugMsg("Unrecognized field type %d\n", field_type);
          }

        }
      }
      continue;
    }
    if (field_type == FieldDescriptor::Type::TYPE_MESSAGE) {
      if (proto_cdk_field->hide_expand) {
        debugMsg("\tpopulateMessageData: field %s had %d children\n",
            field_descriptor->full_name().c_str(), proto_cdk_field->children.size());
        // We hit expand so we should populate recursively.
        populateMessageData(reflection->MutableMessage(message,
              field_descriptor), proto_cdk_field->children);
      }
    } else {
      // Get the string associated with the field
      const char* value = ((CDKENTRY*)proto_cdk_field->field_cdk_obj)->info;
      if (value == NULL || strlen(value) == 0) {
        continue;
      }

      // Handle based on the type
      switch(field_type) {
        case FieldDescriptor::Type::TYPE_DOUBLE:
          reflection->SetDouble(message, field_descriptor, atof(value));
          break;

        case FieldDescriptor::Type::TYPE_FLOAT:
          reflection->SetFloat(message, field_descriptor, atof(value));
          break;

        case FieldDescriptor::Type::TYPE_INT64:
        case FieldDescriptor::Type::TYPE_FIXED64:
        case FieldDescriptor::Type::TYPE_SFIXED64:
        case FieldDescriptor::Type::TYPE_SINT64:
          reflection->SetInt64(message, field_descriptor, atol(value));
          break;
        case FieldDescriptor::Type::TYPE_INT32:
        case FieldDescriptor::Type::TYPE_SFIXED32:
        case FieldDescriptor::Type::TYPE_FIXED32:
        case FieldDescriptor::Type::TYPE_SINT32:
          reflection->SetInt32(message, field_descriptor, atoi(value));
          break;

        case FieldDescriptor::Type::TYPE_UINT64:
          reflection->SetUInt64(message, field_descriptor, atol(value));
          break;
        case FieldDescriptor::Type::TYPE_UINT32:
          reflection->SetUInt32(message, field_descriptor, atoi(value));
          break;

        case FieldDescriptor::Type::TYPE_BOOL:
          {
            std::string lowercase = tolower(std::string(value));
            reflection->SetBool(message,
                field_descriptor,
                strcmp(lowercase.c_str(), "true") == 0 ||
                value[0] == '1');
          }
          break;

        case FieldDescriptor::Type::TYPE_STRING:
          reflection->SetString(message, field_descriptor, std::string(value));
          break;
        case FieldDescriptor::Type::TYPE_BYTES:
          // Assume bytes are base 64 encoded
          {
            std::string binaryProto;
            if (Base64Unescape(value, &binaryProto)) {
              reflection->SetString(message, field_descriptor, binaryProto);
            } else {
              reflection->SetString(message, field_descriptor, value);
            }
            break;
          }

        case FieldDescriptor::Type::TYPE_ENUM:
          {
            const EnumDescriptor* enum_descriptor = field_descriptor->enum_type();
            const EnumValueDescriptor* enum_value_descriptor;
            if (isdigit(value[0])) {
              enum_value_descriptor = enum_descriptor->FindValueByNumber(atoi(value));
            } else {
              enum_value_descriptor = enum_descriptor->FindValueByName(value);
            }
            if (enum_value_descriptor != nullptr) {
              reflection->SetEnum(message, field_descriptor, enum_value_descriptor);
            }
          }
          break;

        default:
          debugMsg("Unrecognized field type %d\n", field_type);
      }

    }
  }
}

void showMultilineMessage(CDKSWINDOW* display, const std::string& message) {
  cleanCDKSwindow(display);
  std::string current_line;
  std::vector<char*> lines;
  for (int i = 0; i < message.size(); i++) {
    if (message[i] == '\n') {
      lines.push_back(strdup(current_line.c_str()));
      current_line.clear();
    } else {
      current_line += message[i];
    }
  }
  if (current_line.size() > 0) {
      lines.push_back(strdup(current_line.c_str()));
  }
  if (lines.size() > 0) {
    setCDKSwindowContents(display, lines.data(), lines.size());
  }

  // Free the memory
  for (char* line: lines) {
    free(line);
  }
}

/**
 * Generate the JSON version of a message.
 */
std::string getJsonMessage(
    const Descriptor* input_descriptor,
    const std::vector<ProtoCDKField*> fields) {
  Message* message = dynamic_message_factory.GetPrototype(input_descriptor)->New();
  // Populate message fields
  populateMessageData(message, fields);

  // Convert to json
  std::string jsonOutput;
  JsonPrintOptions printOptions;
  printOptions.preserve_proto_field_names = true;
  printOptions.always_print_primitive_fields = false;
  printOptions.add_whitespace = true;
  printOptions.always_print_enums_as_ints = false;
  MessageToJsonString(*message, &jsonOutput, printOptions);
  delete message;
  return jsonOutput;
}


/**
 * Write a script that can perform Rpc requests against a particular dependency.
 */
std::string exportScript(
    std::string request_template,
    CDKSCREEN* cdk_screen,
    const MethodDescriptor* method_descriptor,
    const std::vector<ProtoCDKField*> fields,
    const std::vector<const char*> proto_dirs,
    int user_choose_filename = 0) {
  static std::regex placeholder_expression("###\\{([-_ a-zA-Z0-9]+)\\}");
  // Ask for request template path if it was not given on the command line, or
  // the one given on the command line is not a valid file.
  std::error_code ec;
  while (request_template.empty() || !std::filesystem::is_regular_file(request_template, ec)) {
    request_template = getInput(
        cdk_screen,
        /*title=*/"Please enter a valid path to the request template file. "
        "In the future, you can specify this on the command line.",
        /*label=*/"Request template file: ");
  }

  // Read the template file and check for anything that matches the general
  // pattern ###{}. These are variables that will be filled in by asking the
  // user for them, unless it's one of the special variables with a reserved name.
  std::vector<std::string> script_lines;
  std::ifstream template_stream(request_template);
  std::unordered_set<std::string> variables;
  for (std::string line; std::getline(template_stream, line);) {
    script_lines.push_back(line);
    std::smatch match;
    while (std::regex_search (line,match,placeholder_expression)) {
      // Grab the first group which is the name of the variable.
      variables.insert(match[1].str());
      line = match.suffix().str();
    }
  }

  static std::unordered_set<std::string> special_variables({
    "JSON_REQUEST",
    "BASE64_PROTO_REQUEST",
    "PROTO_DIRS",
    "SERVICE_PROTO_FILE",
    "REQUEST_PROTO_FILE",
    "RESPONSE_PROTO_FILE",
    "FULL_SERVICE_NAME",
    "SERVICE_NAME",
    "FULL_METHOD_NAME",
    "METHOD_NAME",
    "FULL_REQUEST_NAME",
    "FULL_RESPONSE_NAME"});
  std::unordered_map<std::string, std::string> variable_values;
  for (std::string variable : variables) {
    // Ask for any variable without a reserved name.
    if (special_variables.find(variable) == special_variables.end()) {
      std::string prompt = "Please enter " + variable + ":";
      std::string variable_value = getInput(
          cdk_screen,
          /*title=*/prompt.c_str(),
          /*label=*/"");
      variable_values[variable] = variable_value;
    } else {
      // Iterate the system generated variables in the template file and create
      // them if they are asked for.
      if (variable == "JSON_REQUEST") {
        std::string json_message = getJsonMessage(method_descriptor->input_type(), fields);
        variable_values[variable] = json_message;
      } else if (variable == "BASE64_PROTO_REQUEST") {
        Message* message = dynamic_message_factory.GetPrototype(method_descriptor->input_type())->New();
        // Populate message fields
        populateMessageData(message, fields);
        std::string base64_binary_proto;
        Base64Escape(message->SerializeAsString(), &base64_binary_proto);
        variable_values[variable] = base64_binary_proto;
      } else if (variable == "PROTO_DIRS") {
        std::string proto_dirs_cat;
        for (const char* proto_dir: proto_dirs) {
          proto_dirs_cat += proto_dir;
          proto_dirs_cat += "\n";
        }
        proto_dirs_cat.pop_back();
        variable_values[variable] = proto_dirs_cat;
      } else if (variable == "SERVICE_PROTO_FILE") {
        variable_values[variable] = method_descriptor->file()->name();
      } else if (variable == "REQUEST_PROTO_FILE") {
        variable_values[variable] = method_descriptor->input_type()->file()->name();
      } else if (variable == "RESPONSE_PROTO_FILE") {
        variable_values[variable] = method_descriptor->output_type()->file()->name();
      } else if (variable == "FULL_SERVICE_NAME") {
        variable_values[variable] = method_descriptor->service()->full_name();
      } else if (variable == "SERVICE_NAME") {
        variable_values[variable] = method_descriptor->service()->name();
      } else if (variable == "FULL_METHOD_NAME") {
        variable_values[variable] = method_descriptor->full_name();
      } else if (variable == "METHOD_NAME") {
        variable_values[variable] = method_descriptor->name();
      } else if (variable == "FULL_REQUEST_NAME") {
        variable_values[variable] = method_descriptor->input_type()->full_name();
      } else if (variable == "FULL_RESPONSE_NAME") {
        variable_values[variable] = method_descriptor->output_type()->full_name();
      }
    }
  }

  // Ask for filename if caller said we should ask.
  std::string filename;
  if (user_choose_filename) {
    filename = getInput(
        cdk_screen,
        /*title=*/"Please enter the desired filename of the exported script:",
        /*label=*/"Filename: ");

    while (filename.empty()) {
      filename = getInput(
          cdk_screen,
          /*title=*/"Please enter the desired filename of the exported script:",
          /*label=*/"Filename: ");
    }

    // Expand wildcards like ~ and variables in the filename, so that we can
    // support paths like `~/Desktop/get_merchant.sh`.
    wordexp_t word_expansion;
    int error = wordexp(filename.c_str(), &word_expansion, 0);
    if (error) {
      debugMsg("wordexp failed with error code %d. Falling back to non-expanded "
          "filename.\n", error);
      if (error == WRDE_NOSPACE) {
        wordfree(&word_expansion);
      }
    } else if (word_expansion.we_wordc != 1) {
      debugMsg("wordexp expanded to %lu != 1 words. "
          "Falling back to non-expanded filename.\n", word_expansion.we_wordc);
      wordfree(&word_expansion);
    } else {
      filename = word_expansion.we_wordv[0];
      wordfree(&word_expansion);
    }
  } else {
    char* temp = strdup("/tmp/RpcExplorer.XXXXXX");
    int fd = mkstemp(temp);
    close(fd);
    filename = temp;
    free(temp);
  }

  // Disallow single quotes after expansion because they will break our
  // system commands below.
  if (filename.find("'") != std::string::npos) {
    return "";
  }

  std::ofstream script_file(filename);
  for (std::string line: script_lines) {
    // Single pass replacement of any variables
    std::string output_line;
    std::smatch match;
    while (std::regex_search (line,match,placeholder_expression)) {
      // Copy the part of the line before the match
      output_line += match.prefix().str();
      // Add the substitution based on the variable name.
      output_line += variable_values[match[1].str()];
      // Update the line to the remainder of the line
      line = match.suffix().str();
    }
    output_line += line;

    // Write the line out to the file.
    script_file << output_line << std::endl;
  }
  return filename;
}

/**
 * Makes a lower case copy of an std::string.  Note that this does not work on
 * the non-ascii subset of Unicode, but this is assumed to be safe for service
 * names and method names.
 * https://stackoverflow.com/a/313990/391161
 */
std::string tolower(std::string input) {
  std::transform(input.begin(), input.end(), input.begin(),
    [](unsigned char c){ return std::tolower(c); });
  return input;
}

/**
 * Split a string based on whitespace.
 * https://stackoverflow.com/a/14267455/391161
 */
std::vector<std::string> split(const char* input) {
  std::vector<std::string> tokens;
  // Trim leading whitespace
  while (*input == ' ' || *input == '\t') {
    input++;
  }

  while (*input) {
    std::string token;
    while (*input && *input != ' ' && *input != '\t') {
      token.push_back(*input);
      input++;
    }

    // We found whitespace or end of string so we exit the inner loop.
    tokens.push_back(token);

    // Iterate until we are no longer pointing at whitespace.
    while (*input == ' ' || *input == '\t') {
      input++;
    }
  }
  return tokens;
}

/**
 * Helper function to tell an object it is no longer focused.
 */
static void unsetFocus(CDKOBJS* obj) {
   obj->hasFocus = 0;
   obj->fn->unfocusObj(obj);
}

/**
 * Helper function to tell an object it is now focused.
 */
static void setFocus(CDKOBJS* obj) {
   obj->hasFocus = 1;
   obj->fn->focusObj(obj);
   curs_set(1);
}


/**
 * Show an information panel.
 */
void showInfoPanel(CDKSCREEN* cdk_screen, const std::string& info) {
  CDKSWINDOW* window = newCDKSwindow(cdk_screen, LEFT, TOP, num_rows - ROWS_FOR_ONSCREEN_HELP,
      num_cols / 2, "", 100000, 1, 0);
  showMultilineMessage(window, info);
  debugMsg("showInfoPanel: after showMultilineMessage.\n");
  activateCDKSwindow(window, NULL);
  debugMsg("showInfoPanel: after activateCDKSwindow.\n");
  destroyCDKSwindow(window);
  debugMsg("showInfoPanel: after destroyCDKSwindow.\n");
  // Restore previous contents of screen.
  refreshCDKScreen (cdk_screen);
}
/**
 * Exec and get stdout.
 * https://stackoverflow.com/questions/478898/how-do-i-execute-a-command-and-get-the-output-of-the-command-within-c-using-po
 */
std::string exec_cmd(const char* cmd) {
    char buffer[128];
    std::string result = "";
    FILE* pipe = popen(cmd, "r");
    if (!pipe) throw std::runtime_error("popen() failed!");
    try {
        while (fgets(buffer, sizeof buffer, pipe) != NULL) {
            result += buffer;
        }
    } catch (...) {
        pclose(pipe);
        throw;
    }
    pclose(pipe);
    return result;
}

/**
 * Ask the user for input.
 */
std::string getInput(CDKSCREEN* cdk_screen, const char* title, const char* label) {
  char* userValue;
  CDKENTRY * entry = newCDKEntry(cdk_screen,
      LEFT, CENTER,
      title,
      label, A_NORMAL, ' ', vMIXED,
      128, 0, 256,
      TRUE, FALSE);
  userValue = activateCDKEntry(entry, NULL);
  while (entry->exitType != vNORMAL)
  {
    userValue = activateCDKEntry(entry, NULL);
  }
  // Construct this before destruction, so we save a copy of the C String.
  std::string retVal(userValue);
  destroyCDKEntry(entry);
  refreshCDKScreen (cdk_screen);
  return retVal;
}

/**
 * Subclasses represent a full page cureses display and handle all rendering
 * and interactions with the user.
 */
class UserFacingPage {
public:
  /**
   * Consume a key from the user. This will be invoked after global hotkeys are
   * examined and do not consume the key.
   *
   * Returning True means completed, so the caller should pop this screen off
   * of the stack and destroy it.
   */
  virtual bool handleInput(int keyCode, int functionKey) = 0;
  virtual void redraw() = 0;
  virtual ~UserFacingPage() {}

  /**
   * Get the active object, needed for getting user input.
   */
  virtual CDKOBJS* getCDKActiveObject() = 0;
};

/**
 * A page where users can build requests interactively, make requests, and
 * export scripts.
 */
class RequestBuilderPage : public UserFacingPage {
public:
  /**
   * Constructor. Will crash program on failures.
   */
  RequestBuilderPage(const MethodDescriptor* method_descriptor, const Options& options)
      : method_descriptor(method_descriptor)
      , input_descriptor(method_descriptor->input_type())
      , options(options) {
    cdk_screen = initCDKScreen (NULL);
    // Initially, the virtual and physical screen sizes are the same.
    min_row_to_display = 0;
    max_row_to_display = num_rows - ROWS_FOR_ONSCREEN_HELP;
    // Create the initial set of entries based on the top level field of the
    // descriptor.
    for (int i = 0; i < input_descriptor->field_count(); i++) {
      const FieldDescriptor* field_descriptor = input_descriptor->field(i);
      root_proto_cdk_fields.push_back(addDescriptorToDisplay(field_descriptor, 0, i));
    }
    CDKBUTTON* request_button =
        newCDKButton(cdk_screen, LEFT, proto_cdk_fields.size() + 2, "Make Request",[](struct SButton *button){}, 0, 0);
    CDKBUTTON* export_button =
        newCDKButton(cdk_screen, strlen("Make Request") + 2, proto_cdk_fields.size() + 2, "Export Script",[](struct SButton *button){}, 0, 0);

    // Add the buttons to the list
    ProtoCDKField* proto_cdk_field = new ProtoCDKField();
    proto_cdk_field->field_cdk_obj = (CDKOBJS*) request_button;
    proto_cdk_field->field_cdk_type = REQUEST_BUTTON;
    proto_cdk_field->tab_index = 0;
    unsetFocus(proto_cdk_field->field_cdk_obj);
    proto_cdk_fields.push_back(proto_cdk_field);

    proto_cdk_field = new ProtoCDKField();
    proto_cdk_field->field_cdk_obj = (CDKOBJS*) export_button;
    proto_cdk_field->field_cdk_type = EXPORT_BUTTON;
    proto_cdk_field->tab_index = 0;
    unsetFocus(proto_cdk_field->field_cdk_obj);
    proto_cdk_fields.push_back(proto_cdk_field);

    redrawProtoCdkFields(0);
    createHelpWindow();

    // Create the display on the right and initialize with empty JSON
    json_display = newCDKSwindow(cdk_screen, RIGHT, TOP, num_rows - ROWS_FOR_ONSCREEN_HELP, num_cols / 2, "", 1000, 1, 0);
    drawCDKSwindow(json_display, 1);
    updateJsonDisplay();

    // Set up the current focus
    index = 0;
    cur_object = setCDKFocusCurrent(cdk_screen, proto_cdk_fields[index]->field_cdk_obj);
    setFocus(cur_object);
  }

  /**
   * Destructor.
   */
  ~RequestBuilderPage() {
    for (ProtoCDKField* proto_cdk_field : proto_cdk_fields) {
      if (proto_cdk_field->field_cdk_obj != NULL) {
        // Assuming it is always a button is not quite kosher, but hopefully
        // the macros will find the true type and do the right thing.
        destroyCDKObject((CDKBUTTON*)proto_cdk_field->field_cdk_obj);
        proto_cdk_field->field_cdk_obj = NULL;
      }
      if (proto_cdk_field->field_label_string != NULL) {
        delete[] proto_cdk_field->field_label_string;
        proto_cdk_field->field_label_string = NULL;
      }
      if (proto_cdk_field->field_cdk_label != NULL) {
        destroyCDKObject(proto_cdk_field->field_cdk_label);
        proto_cdk_field->field_cdk_label = NULL;
      }
      delete proto_cdk_field;
    }
    proto_cdk_fields.clear();

    destroyCDKSwindow(json_display);
    json_display = NULL;

    if (help_window != NULL) {
      destroyCDKSwindow(help_window);
      help_window = NULL;
    }

    destroyCDKScreen(cdk_screen);
    cdk_screen = NULL;
  }

  virtual bool handleInput(int key_code, int function_key) {
    ProtoCDKField* proto_cdk_field = proto_cdk_fields[index];
    switch (key_code){
      case KEY_BTAB:
      case KEY_UP:
        focusPrevious();
        break;
      case KEY_TAB:
      case KEY_DOWN:
        focusNext();
        break;
      case 153: // Alt-H
      case KEY_F1:
        if (proto_cdk_field->field_cdk_type != ENTRY &&
            proto_cdk_field->field_cdk_type != EXPAND_BUTTON &&
            proto_cdk_field->field_cdk_type != ADD_BUTTON) {
          break;
        }
        // Show the definition for the current field if it's an enum or message
        if (proto_cdk_field->field_descriptor->type() == FieldDescriptor::Type::TYPE_ENUM) {
          DebugStringOptions options;
          options.include_comments = true;
          std::string debugString = proto_cdk_field->field_descriptor->enum_type()->DebugStringWithOptions(options);
          showInfoPanel(cdk_screen, debugString);
        } else if (proto_cdk_field->field_descriptor->type() == FieldDescriptor::Type::TYPE_MESSAGE) {
          DebugStringOptions options;
          options.include_comments = true;
          std::string debugString = proto_cdk_field->field_descriptor->message_type()->DebugStringWithOptions(options);
          showInfoPanel(cdk_screen, debugString);
        } else {
          // Show the definition of the enclosing message
          DebugStringOptions options;
          options.include_comments = true;
          std::string debugString = proto_cdk_field->field_descriptor->containing_type()->DebugStringWithOptions(options);
          showInfoPanel(cdk_screen, debugString);
        }
        break;
      case KEY_ENTER:
        // Check if we are a button.
        proto_cdk_field = proto_cdk_fields[index];
        if (proto_cdk_field->field_cdk_type == EXPAND_BUTTON) {
          // Hide the button
          proto_cdk_field->hide_expand = 1;
          const Descriptor* input_descriptor = proto_cdk_field->field_descriptor->message_type();
          debugMsg("Added children to field %s\n", input_descriptor->full_name().c_str());
          for (int i = 0; i < input_descriptor->field_count(); i++) {
            const FieldDescriptor* field_descriptor = input_descriptor->field(i);
            debugMsg("Adding child %s\n", field_descriptor->full_name().c_str());
            proto_cdk_field->children.push_back(
                addDescriptorToDisplay(field_descriptor, proto_cdk_field->tab_index + 1, index + 1 + i));
          }
          debugMsg("Added %d children to field. Ending children %d\n",
              input_descriptor->field_count(),
              proto_cdk_field->children.size());
          redrawProtoCdkFields(index + 1);
          ProtoCDKField* prev = proto_cdk_field;
          focusNext();
          destroyCDKButton((CDKBUTTON*)prev->field_cdk_obj);
          prev->field_cdk_obj = NULL;
          setFocus(cur_object);
        } else if (proto_cdk_field->field_cdk_type == ADD_BUTTON) {
          // Add a copy of the repeated field
          proto_cdk_field->children.push_back(
              addDescriptorToDisplay(
                proto_cdk_field->field_descriptor,
                proto_cdk_field->tab_index + 1, index + 1, /*child_of_repeated*/1));
          redrawProtoCdkFields(index + 1);
          focusNext();
        } else if (proto_cdk_field->field_cdk_type == REQUEST_BUTTON) {
          std::string path =
              exportScript(options.request_template, cdk_screen, method_descriptor, root_proto_cdk_fields, options.protoPaths);

          char cmd_buffer[1024];
          // Update permissions
          snprintf(cmd_buffer, sizeof(cmd_buffer), "chmod u+x '%s'", path.c_str());
          system(cmd_buffer);
          // Execute script
          snprintf(cmd_buffer, sizeof(cmd_buffer), "'%s' 2>&1", path.c_str());
          debugMsg("Start executing generated script %s.\n", cmd_buffer);
          std::string cmd_output = exec_cmd(cmd_buffer);
          debugMsg("Finished executing script.\n");
          debugMsg("Script output:\n'''\n%s\n'''\n", cmd_output.c_str());
          // Clean up script
          snprintf(cmd_buffer, sizeof(cmd_buffer), "rm -f '%s'", path.c_str());
          system(cmd_buffer);
          showInfoPanel(cdk_screen, cmd_output);
          debugMsg("Finished showing command output.");
        } else if (proto_cdk_field->field_cdk_type == EXPORT_BUTTON) {
          std::string path =
              exportScript(options.request_template, cdk_screen, method_descriptor, root_proto_cdk_fields, options.protoPaths, 1);

          // Path contained a single quote.
          // This check is not needed above because we did not try to export to
          // a filename that the user provided.
          if (path.empty()) {
            showInfoPanel(cdk_screen, "Filenames cannot contain single quote characters.");
            break;
          }

          char cmd_buffer[1024];
          // Update permissions
          snprintf(cmd_buffer, sizeof(cmd_buffer), "chmod u+x '%s'", path.c_str());
          system(cmd_buffer);

          char cmd_output[1024];
          snprintf(cmd_output, sizeof(cmd_output), "Wrote script file to '%s'!", path.c_str());
          showInfoPanel(cdk_screen, cmd_output);
        }
        break;
      case CDK_REFRESH:
        /* redraw screen */
        redraw();
        break;
      case KEY_ESC:
      case KEY_F7:
        return true;
      default:
        InjectObj(cur_object, key_code);
    }
    debugMsg("index = %d\n", index);
    updateJsonDisplay();
    setFocus(cur_object);
    return false;
  }

  virtual void redraw() {
    // This is needed in case the window is resized.
    max_row_to_display = min_row_to_display + (num_rows - ROWS_FOR_ONSCREEN_HELP);
    // Ensure that we scroll the window down enough to see the currently
    // selected object.
    while (proto_cdk_fields[index]->field_virtual_row > max_row_to_display) {
      max_row_to_display++;
      min_row_to_display++;
    }

    // Destroy help first so that the destruction of help does not erase the
    // fields that were in its former location.
    if (help_window != NULL) {
      destroyCDKSwindow(help_window);
    }

    // Recreate the JSON display to handle resizing events.
    if (json_display != NULL) {
      destroyCDKSwindow(json_display);
      json_display = NULL;
    }

    redrawProtoCdkFields(0);
    json_display = newCDKSwindow(cdk_screen, RIGHT, TOP, num_rows - ROWS_FOR_ONSCREEN_HELP,
        num_cols / 2, "", 1000, 1, 0);
    drawCDKSwindow(json_display, 1);
    updateJsonDisplay();

    createHelpWindow();
    cur_object = setCDKFocusCurrent(cdk_screen, proto_cdk_fields[index]->field_cdk_obj);
    if (cur_object) {
      setFocus(cur_object);
    }
  };

  virtual CDKOBJS* getCDKActiveObject() {
    return cur_object;
  }
private:
  /**
   * The screen used for drawing CDK objects.
   */
  CDKSCREEN* cdk_screen;

  /**
   * The window on the right used for displaying the JSON representation of the
   * constructed proto.
   */
  CDKSWINDOW* json_display;

  /**
   * The set of fields associated with the top-level fields of the message we are constructing.
   */
  std::vector<ProtoCDKField*> root_proto_cdk_fields;

  /**
   * The set of all fields that are currently on the screen, including buttons.
   */
  std::vector<ProtoCDKField*> proto_cdk_fields;

  /**
   * The index of the current object in proto_cdk_fields.
   */
  int index;

  /**
   * The currently focused object, which will receive events.
   */
  CDKOBJS *cur_object;

  /**
   * The minimum row of the virtual display to render, for scrolling.
   */
  int min_row_to_display;

  /**
   * The maximum row of the virtual display to rende, for scrolling.
   */
  int max_row_to_display;

  /**
   * The window where help text will be drawn.
   */
  CDKSWINDOW* help_window;

  /**
   * The descriptor of the Rpc method we are constructing a request for.
   */
  const MethodDescriptor* method_descriptor;

  /**
   * The message descriptor of the input type of the method_descriptor.
   */
  const Descriptor* input_descriptor;

  /**
   * The command line options passed into the program.
   */
  const Options& options;

  /**
   * Add the given field descriptor to the given position in the display.
   */
  ProtoCDKField* addDescriptorToDisplay(
      const FieldDescriptor* field_descriptor,
      int tab_index,
      int insert_before,
      int child_of_repeated = 0) {
    static const size_t field_label_length = 1024;
    char* field_name =  strdup(field_descriptor->name().c_str());
    FieldDescriptor::Type field_type = field_descriptor->type();
    ProtoCDKField* proto_cdk_field = new ProtoCDKField();
    proto_cdk_field->field_descriptor = field_descriptor;
    proto_cdk_field->field_label_string = new char[field_label_length];
    proto_cdk_field->field_cdk_label = NULL;
    proto_cdk_field->field_cdk_obj = NULL;
    proto_cdk_field->hide_expand = 0;
    proto_cdk_field->field_cached_value = NULL;

    if (field_descriptor->is_repeated() && !child_of_repeated) {
      proto_cdk_field->field_cdk_type = ADD_BUTTON;
    } else {
      if (field_type != FieldDescriptor::Type::TYPE_MESSAGE) {
        proto_cdk_field->field_cdk_type = ENTRY;
      } else {
        proto_cdk_field->field_cdk_type = EXPAND_BUTTON;
      }
    }

    char* label = proto_cdk_field->field_label_string;
    if (field_type == FieldDescriptor::Type::TYPE_ENUM) {
      snprintf(label, field_label_length, "%s %s:",
          field_descriptor->enum_type()->full_name().c_str(), field_name);
      if (strlen(label) > num_cols / 2 - 6) {
        snprintf(label, field_label_length, "%s %s:",
            field_descriptor->enum_type()->name().c_str(), field_name);
      }
    } else if (field_type == FieldDescriptor::Type::TYPE_MESSAGE) {
      snprintf(label, field_label_length, "%s %s:", field_descriptor->message_type()->full_name().c_str(), field_name);
      // Label is too long, truncate it.
      if (strlen(label) > num_cols / 2 - 6) {
        snprintf(label, field_label_length, "%s %s:",
            field_descriptor->message_type()->name().c_str(), field_name);
      }
    } else {
      snprintf(label, field_label_length, "%s %s:", field_descriptor->type_name(), field_name);
    }

    proto_cdk_field->field_descriptor = field_descriptor;
    proto_cdk_field->tab_index = tab_index;
    proto_cdk_fields.insert(proto_cdk_fields.begin() + insert_before, proto_cdk_field);

    free(field_name);
    return proto_cdk_field;
  }

  void redrawProtoCdkFields(int insert_before) {
    // Erase before creating, to avoid erasing after creating.
    for (int i = insert_before; i < proto_cdk_fields.size(); i++) {
      ProtoCDKField* proto_cdk_field = proto_cdk_fields[i];

      // Handle the special buttons without labels first
      if (proto_cdk_field->field_cdk_type == REQUEST_BUTTON) {
        if (proto_cdk_field->field_cdk_obj != NULL) {
          destroyCDKObject((CDKBUTTON*)proto_cdk_field->field_cdk_obj);
          proto_cdk_field->field_cdk_obj = NULL;
        }
        continue;
      } else if (proto_cdk_field->field_cdk_type == EXPORT_BUTTON) {
        if (proto_cdk_field->field_cdk_obj != NULL) {
          destroyCDKObject((CDKBUTTON*)proto_cdk_field->field_cdk_obj);
          proto_cdk_field->field_cdk_obj = NULL;
        }
        continue;
      }

      if (proto_cdk_field->field_cdk_label != NULL) {
        destroyCDKObject((CDKLABEL*)proto_cdk_field->field_cdk_label);
        proto_cdk_field->field_cdk_label = NULL;
      }
      if (proto_cdk_field->field_cdk_type == ENTRY) {
        if (proto_cdk_field->field_cdk_obj != NULL) {
          proto_cdk_field->field_cached_value =
              strdup(((CDKENTRY*)proto_cdk_field->field_cdk_obj)->info);
          destroyCDKObject((CDKENTRY*)proto_cdk_field->field_cdk_obj);
          proto_cdk_field->field_cdk_obj = NULL;
        }
      } else if (proto_cdk_field->field_cdk_type == EXPAND_BUTTON) {
        if (proto_cdk_field->field_cdk_obj != NULL) {
          destroyCDKObject((CDKBUTTON*)proto_cdk_field->field_cdk_obj);
          proto_cdk_field->field_cdk_obj = NULL;
        }
      } else if (proto_cdk_field->field_cdk_type == ADD_BUTTON) {
        if (proto_cdk_field->field_cdk_obj != NULL) {
          destroyCDKObject((CDKBUTTON*)proto_cdk_field->field_cdk_obj);
          proto_cdk_field->field_cdk_obj = NULL;
        }
      }
    }

    // Create and draw, since moving is not working in CDK
    for (int i = insert_before; i < proto_cdk_fields.size(); i++) {
      ProtoCDKField* proto_cdk_field = proto_cdk_fields[i];

      // Handle the special buttons without labels first
      if (proto_cdk_field->field_cdk_type == REQUEST_BUTTON) {
        proto_cdk_field->field_virtual_row = proto_cdk_fields.size();
        CDKBUTTON* request_button =
            newCDKButton(cdk_screen, LEFT, proto_cdk_field->field_virtual_row - min_row_to_display,
                "Make Request",[](struct SButton *button){}, 0, 0);
        proto_cdk_field->field_cdk_obj = (CDKOBJS*) request_button;
        if (proto_cdk_field->field_virtual_row >= min_row_to_display && proto_cdk_field->field_virtual_row <= max_row_to_display) {
          drawCDKButton(request_button, 0);
          unsetFocus(proto_cdk_field->field_cdk_obj);
        }
        continue;
      } else if (proto_cdk_field->field_cdk_type == EXPORT_BUTTON) {
        proto_cdk_field->field_virtual_row = proto_cdk_fields.size();
        CDKBUTTON* export_button =
            newCDKButton(cdk_screen, strlen("Make Request") + 2, proto_cdk_field->field_virtual_row - min_row_to_display,
                "Export Script",[](struct SButton *button){}, 0, 0);
        proto_cdk_field->field_cdk_obj = (CDKOBJS*) export_button;
        if (proto_cdk_field->field_virtual_row >= min_row_to_display && proto_cdk_field->field_virtual_row <= max_row_to_display) {
          drawCDKButton(export_button, 0);
          unsetFocus(proto_cdk_field->field_cdk_obj);
        }
        continue;
      }


      int xpos = 2 * proto_cdk_field->tab_index;
      proto_cdk_field->field_virtual_row = i;
      int ypos = i - min_row_to_display;
      if (ypos < 0) ypos = 0;
      proto_cdk_field->field_cdk_label =
          newCDKLabel(cdk_screen, xpos, ypos, &proto_cdk_field->field_label_string, 1, 0, 0);
      if (proto_cdk_field->field_virtual_row >= min_row_to_display &&
          proto_cdk_field->field_virtual_row <= max_row_to_display) {
        drawCDKLabel(proto_cdk_field->field_cdk_label, 0);
      }
      xpos += strlen(proto_cdk_field->field_label_string) + 1;
      int width = num_cols / 2 - xpos - 1;
      if (proto_cdk_field->field_cdk_type == ENTRY) {
        CDKENTRY* entry = newCDKEntry (cdk_screen,
            xpos, ypos,
            /*title=*/"", /*label=*/"", A_NORMAL, '_', vMIXED,
            width, 0, 256,
            FALSE, FALSE);
        // The use of this cached value is unfortunate, but necessary because
        // the move APIs for CDK seem to be noops, as far as hqin can tell.
        if (proto_cdk_field->field_cached_value != NULL) {
          setCDKEntryValue(entry, proto_cdk_field->field_cached_value);
          free(proto_cdk_field->field_cached_value);
          proto_cdk_field->field_cached_value= NULL;
        }
        if (proto_cdk_field->field_virtual_row >= min_row_to_display &&
            proto_cdk_field->field_virtual_row <= max_row_to_display) {
          drawCDKEntry(entry, 1);
        }
        proto_cdk_field->field_cdk_obj = (CDKOBJS*) entry;
      } else if (proto_cdk_field->field_cdk_type == EXPAND_BUTTON) {
        if (!proto_cdk_field->hide_expand) {
          CDKBUTTON* button = newCDKButton(cdk_screen, xpos, ypos, "Expand",[](struct SButton *button){}, 0, 0);
          if (proto_cdk_field->field_virtual_row >= min_row_to_display &&
              proto_cdk_field->field_virtual_row <= max_row_to_display) {
            drawCDKButton(button, 0);
          }
          proto_cdk_field->field_cdk_obj = (CDKOBJS*) button;
        }
      } else if (proto_cdk_field->field_cdk_type == ADD_BUTTON) {
        CDKBUTTON* button = newCDKButton(cdk_screen, xpos, ypos, "Add",[](struct SButton *button){}, 0, 0);
        if (proto_cdk_field->field_virtual_row >= min_row_to_display &&
            proto_cdk_field->field_virtual_row <= max_row_to_display) {
          drawCDKButton(button, 0);
        }
        proto_cdk_field->field_cdk_obj = (CDKOBJS*) button;
      }
      if (proto_cdk_field->field_cdk_obj) {
        if (proto_cdk_field->field_virtual_row >= min_row_to_display &&
            proto_cdk_field->field_virtual_row <= max_row_to_display) {
          unsetFocus(proto_cdk_field->field_cdk_obj);
        }
      }
    }
  }

  /**
   * Parse the current proto values and redraw the JSON display on the right.
   */
  void updateJsonDisplay() {
    std::string json_message = getJsonMessage(input_descriptor, root_proto_cdk_fields);
    showMultilineMessage(json_display, json_message);
    unsetFocus((CDKOBJS*)json_display);
  }

  /**
   * Change the focus to the previous item in proto_cdk_fields.
   */
  void focusPrevious() {
    if (cur_object) {
      unsetFocus(cur_object);
    }
    if (index == 0) {
      index = proto_cdk_fields.size() - 1;
    } else {
      index--;
      index %= proto_cdk_fields.size();
    }
    // Scrolling
    bool scrolled = false;
    while (proto_cdk_fields[index]->field_virtual_row < min_row_to_display) {
      scrolled = true;
      max_row_to_display--;
      min_row_to_display--;
    }
    while (proto_cdk_fields[index]->field_virtual_row > max_row_to_display) {
      scrolled = true;
      max_row_to_display++;
      min_row_to_display++;
    }
    if (scrolled) {
      redraw();
      cur_object = proto_cdk_fields[index]->field_cdk_obj;
    }
    // End scrolling

    if (proto_cdk_fields[index]->hide_expand) {
      focusPrevious();
    } else {
      cur_object = setCDKFocusCurrent(cdk_screen, proto_cdk_fields[index]->field_cdk_obj);
      setFocus(cur_object);
    }
  }

  /**
   * Change the focus to the next item in proto_cdk_fields.
   */
  void focusNext() {
    debugMsg("focusNext: cur_object = %p\n", cur_object);
    // If there was a scrolling event and the currently focused object is an
    // already-expanded EXPAND button (which can happen on a recursive call
    // after a scrolling event), then cur_object will be NULL.
    if (cur_object) {
      unsetFocus(cur_object);
    }

    debugMsg("focusNext: indexBefore = %d\n", index);
    index++;
    index %= proto_cdk_fields.size();
    debugMsg("focusNext: indexAfter = %d\n", index);

    // Scrolling
    bool scrolled = false;
    while (proto_cdk_fields[index]->field_virtual_row > max_row_to_display) {
      scrolled = true;
      max_row_to_display++;
      min_row_to_display++;
    }
    while (proto_cdk_fields[index]->field_virtual_row < min_row_to_display) {
      scrolled = true;
      max_row_to_display--;
      min_row_to_display--;
    }
    debugMsg("focusNext: field_virtual_row = %d\n", proto_cdk_fields[index]->field_virtual_row);
    debugMsg("focusNext: max_row_to_display = %d\n", max_row_to_display);
    debugMsg("focusNext: min_row_to_display = %d\n", min_row_to_display);
    if (scrolled) {
      redraw();
      cur_object = proto_cdk_fields[index]->field_cdk_obj;
    }
    debugMsg("focusNext: Finished redraw\n");
    // End scrolling

    if (proto_cdk_fields[index]->hide_expand) {
      debugMsg("focusNext: recursing\n");
      focusNext();
    } else {
      cur_object = setCDKFocusCurrent(cdk_screen, proto_cdk_fields[index]->field_cdk_obj);
      setFocus(cur_object);
    }
  }

  /**
   * Helper function for creating the help section of this page.
   */
  void createHelpWindow() {
    help_window = newCDKSwindow(cdk_screen, LEFT, num_rows - 5, 4, num_cols, "USAGE", 5, 0, 0);
    if (help_window == 0) {
      debugMsg("Failed to create help window. Perhaps screen was too small.\n");
      return;
    }
    std::string help_text =
      "TAB/SHIFT-TAB Move cursor  \tENTER Execute action"
      "\nF1 View proto definition \tESC Back"
      "\nF7 Return to search";
    showMultilineMessage(help_window, help_text);
    drawCDKSwindow(help_window, 0);
  }

};
/**
 * A page where users can search for and select Rpcs.
 */
class RpcSearchPage : public UserFacingPage {
public:
  /**
   * Constructor.
   *
   * Note that the program cannot continue if this constructor fails, it will
   * simply crash the program rather than throwing an exception in the case of
   * failure.
   */
  RpcSearchPage(
      std::vector<UserFacingPage*>& page_stack,
      const std::map<std::string, const MethodDescriptor*>& method_descriptors,
      const Options& options) :
    page_stack(page_stack),
    method_descriptors(method_descriptors),
    options(options) {


    cdk_screen = initCDKScreen (NULL);
    initCDKColor ();

    createHelpWindow();
    createSearchEntryOrDie();
    selection = NULL;
    items = NULL;
  }

  /**
   * Destructor.
   */
  ~RpcSearchPage() {
    if (items != NULL) {
      delete[] items;
    }
    destroyCDKEntry(search_term_entry);
    search_term_entry = NULL;

    if (selection != NULL) {
      destroyCDKSelection(selection);
      selection = NULL;
    }
    if (help_window != NULL ) {
      destroyCDKSwindow(help_window);
    }
    destroyCDKScreen (cdk_screen);
  }

  virtual bool handleInput(int key_code, int function_key) {
    switch (key_code) {
      case KEY_ESC:
        break;
      case KEY_BTAB:
      case KEY_TAB:
        if (cur_object == (CDKOBJS*) search_term_entry) {
          processSearch();
        } else {
          destroyCDKSelection(selection);
          selection = NULL;
          delete[] items;
          items = NULL;
          // Switch back to search_term_entry
          cur_object = (CDKOBJS*) search_term_entry;
          setFocus(cur_object);
          setCDKFocusCurrent(cdk_screen, cur_object);
        }
        break;
      case 153: // Alt-H
      case KEY_F1:
        if (cur_object != (CDKOBJS*) search_term_entry) {
          int selected_index = getCDKSelectionCurrent(selection);
          const MethodDescriptor* method_descriptor = found_descriptors[selected_index];
          DebugStringOptions options;
          options.include_comments = true;
          std::string debugString;
          debugString += method_descriptor->file()->name();
          debugString += "\n";
          debugString += method_descriptor->full_name();
          debugString += "\n";
          debugString += method_descriptor->DebugStringWithOptions(options);
          debugString += "\n";

          debugString += method_descriptor->input_type()->file()->name();
          debugString += "\n";
          debugString += method_descriptor->input_type()->full_name();
          debugString += "\n";
          debugString += method_descriptor->input_type()->DebugStringWithOptions(options);
          debugString += "\n";

          debugString += method_descriptor->output_type()->file()->name();
          debugString += "\n";
          debugString += method_descriptor->output_type()->full_name();
          debugString += "\n";
          debugString += method_descriptor->output_type()->DebugStringWithOptions(options);
          showInfoPanel(cdk_screen, debugString);

          // Switch back to search_term_entry after this returns.
          redraw();
        }
        break;
      case KEY_ENTER:
        if (cur_object == (CDKOBJS*) search_term_entry) {
          processSearch();
        } else {
          int selected_index = getCDKSelectionCurrent(selection);
          // In theory, we could support going back to this screen without recreating  it.
          eraseCDKScreen(cdk_screen);
          // Create the next screen and push it onto the stack.
          RequestBuilderPage* request_builder_page =
              new RequestBuilderPage(found_descriptors[selected_index],
                  options);
          page_stack.push_back(request_builder_page);
        }
        break;
      case 'j':
        if (cur_object != (CDKOBJS*) search_term_entry) {
          InjectObj(cur_object, KEY_DOWN);
        } else {
          InjectObj(cur_object, key_code);
        }
        break;
      case 'k':
        if (cur_object != (CDKOBJS*) search_term_entry) {
          InjectObj(cur_object, KEY_UP);
        } else {
          InjectObj(cur_object, key_code);
        }
        break;
      default:
        InjectObj(cur_object, key_code);
    }
    return false;
  }

  virtual CDKOBJS* getCDKActiveObject() {
    return cur_object;
  }

  virtual void redraw() {
    // Save data and recreate cdk objects to support screen resizing.
    bool in_selection = false;
    int selected_index;
    if (selection != NULL) {
      in_selection = true;
      selected_index = getCDKSelectionCurrent(selection);
    }
    char* search_box_content = strdup(search_term_entry->info);

    // Destroy CDK objects
    destroyCDKEntry(search_term_entry);
    if (in_selection) {
      destroyCDKSelection(selection);
    }

    // Recreate CDK objects
    createSearchEntryOrDie();
    setCDKEntryValue(search_term_entry, search_box_content);
    // Draw again after restoring the value.
    drawCDKEntry(search_term_entry, 1);
    if (in_selection) {
      createAndFocusSelection();
      setCDKSelectionCurrent(selection, selected_index);
      drawCDKSelection(selection, 1);
    }

    free(search_box_content);

    // Redraw help text.
    if (help_window != NULL) {
      destroyCDKSwindow(help_window);
    }
    createHelpWindow();
  };


private:
  /**
   * The display page stack, used for adding pages.
   */
  std::vector<UserFacingPage*>& page_stack;

  /**
   * The screen used for drawing CDK objects.
   */
  CDKSCREEN* cdk_screen;

  /**
   * The field where the search entry will be entered.
   */
  CDKENTRY* search_term_entry;

  /**
   * The window displaying the list of descriptors matching the search.
   */
  CDKSELECTION *selection;

  /**
   * The window where help text will be drawn.
   */
  CDKSWINDOW* help_window;

  /**
   * The items that are part of the selection. Since the sellection does not
   * appear to own this memory and allocates new memory instead, we must own it
   * instead.
   */
  const char** items;

  /**
   * The currently active GUI object that is receiving input.
   */
  CDKOBJS* cur_object;

  /**
   * The method descriptors to search over.
   */
  const std::map<std::string, const MethodDescriptor*>& method_descriptors;

  /**
   * The descriptors that match the search term.
   */
  std::vector<const MethodDescriptor*> found_descriptors;

  /**
   * The command line options passed into the program.
   */
  const Options& options;

  /**
   * Perform actual search based on search box input and activate selection.
   */
  void processSearch() {
    char *search_term = search_term_entry->info;
    // Perform the search and populate the selection.
    search_term = strdup(tolower(search_term).c_str());
    std::vector<std::string> tokens = split(search_term);
    free(search_term);

    // Perform the search and populate the scrolling selection.
    found_descriptors.clear();
    for (auto const& method : method_descriptors) {
      bool match = true;
      for (auto const& token : tokens) {
        if (method.first.find(token) == std::string::npos) {
          match = false;
        }
      }
      if (match) {
        found_descriptors.push_back(method.second);
      }
    }

    // Abort early if there are no results.
    if (found_descriptors.empty()) {
      showInfoPanel(cdk_screen, "No results found!");
      return;
    }

    // Populate the selection
    items = new const char*[found_descriptors.size()];
    for (int i = 0; i < found_descriptors.size(); i++) {
      items[i] = found_descriptors[i]->full_name().c_str();
    }
    createAndFocusSelection();
  }

  /**
   * Helper function for creating the help section of this page.
   */
  void createHelpWindow() {
    help_window = newCDKSwindow(cdk_screen, LEFT, num_rows - 5, 4, num_cols, "USAGE", 5, 0, 0);
    if (help_window == 0) {
      debugMsg("Failed to create help window. Perhaps screen was too small.\n");
      return;
    }
    std::string help_text =
        "Type a Rpc name and press ENTER to search."
        "\nUP/DOWN Select\tENTER Choose\tTAB Move between windows"
        "\nF1 View Rpc definition";
    showMultilineMessage(help_window, help_text);
    drawCDKSwindow(help_window, 0);
  }

  /**
   * Helper function for creating the search term entry.
   */
  void createSearchEntryOrDie() {
    /* Create the entry field widget. */
    search_term_entry = newCDKEntry (cdk_screen,
        LEFT, TOP,
        /*title=*/"", /*label=*/"", A_NORMAL, ' ', vMIXED,
        num_cols, 0, 256,
        TRUE, FALSE);

    /* Is the widget null? */
    if (search_term_entry == 0)
    {
      /* Clean up. */
      destroyCDKScreen(cdk_screen);
      endCDK ();

      printf ("Cannot create the entry box. Is the window too small?\n");
      exit(1);
    }
    drawCDKEntry(search_term_entry, 1);
    cur_object = (CDKOBJS*) search_term_entry;
  }

  /**
   * Helper function for creating the selection box. Note that this assumes
   * that items already exists and is populated with the contents of
   * found_descriptors.
   */
  void createAndFocusSelection() {
    static const char *choices[] = { "", "" };
    selection = newCDKSelection (cdk_screen,
        LEFT,
        3,
        CDKparsePosition("RIGHT"),
        std::min(num_rows * 2 / 3, num_rows - ROWS_FOR_ONSCREEN_HELP - 1),
        num_cols,
        /*title=*/"",
        (CDK_CSTRING2) items,
        found_descriptors.size(),
        (CDK_CSTRING2) choices, 2,
        A_REVERSE,
        TRUE,
        FALSE);

    /* Is the selection list null? */
    if (selection == 0)
    {
      /* Exit CDK. */
      destroyCDKScreen (cdk_screen);
      endCDK ();

      printf ("Cannot to create the selection list.\n");
      printf ("Is the window too small?\n");
      exit(1);
    }
    cur_object = setCDKFocusCurrent(cdk_screen, (CDKOBJS*) selection);
    unsetFocus(cur_object);
    setFocus(cur_object);
  }
};

/**
 * Make sure there's enough of a window to render a meaningful UI, and exit if
 * that is not the case.
 */
void ensureMinWindowSize() {
  if (num_rows < ROWS_FOR_ONSCREEN_HELP + 5) {
    endCDK ();
    printf("\n\nWindow is too small to render RpcExplorer.\n");
    exit(0);
  }
}

/**
 * Run the interactive user interface to search for protos based on curses.
 */
void runCursesInterface(Options& options,
    const std::map<std::string, const MethodDescriptor*>& method_descriptors) {
  /* Reduce the escape delay so that Esc is more snappy.  */
  setenv("ESCDELAY","1", 1);
  initscr();

  static const char* should_use_default_colors = getenv("RPC_EXPLORER_USE_DEFAULT_COLORS");
  if (should_use_default_colors != NULL && strcmp("true", tolower(std::string(should_use_default_colors)).c_str()) == 0) {
    use_default_colors();
  }

  getmaxyx(stdscr, num_rows, num_cols);
  ensureMinWindowSize();


  // A stack containing the current screens to display to the user. The top
  // screen is the one that is actually rendered and receives input. A screen
  // is responsible for rendering itself on construction, and should only be
  // constructed once there is enough information for rendering.
  std::vector<UserFacingPage*> page_stack;
  RpcSearchPage* search_page = new RpcSearchPage(page_stack, method_descriptors, options);
  page_stack.push_back(search_page);

  // Main user input loop.
  int key_code;
  int function_key;

  while ((key_code = getchCDKObject(
          (CDKOBJS*)page_stack.back()->getCDKActiveObject(), &function_key))) {
    switch (key_code) {
      // Global hotkeys should be added here.
      case KEY_F8:
        for (UserFacingPage* page : page_stack) {
          delete page;
        }
        {
          page_stack.clear();
          RpcSearchPage* search_page = new RpcSearchPage(page_stack, method_descriptors, options);
          page_stack.push_back(search_page);
        }
        break;
      case KEY_RESIZE:
        getmaxyx(stdscr, num_rows, num_cols);
        ensureMinWindowSize();
        if (!page_stack.empty()) {
          page_stack.back()->redraw();
        }
        break;

      default:
        debugMsg("Handling input (key_code = %d, function_key = %d)", key_code, function_key);
        if (page_stack.back()->handleInput(key_code, function_key)) {
          delete page_stack.back();
          page_stack.pop_back();
          if (!page_stack.empty()) {
            page_stack.back()->redraw();
          }
        }
    }
  }

  /* Clean up and exit. */
  endCDK ();
  exit(0);
}

/**
 * Search for services and methods and generate bash scripts for invoking them
 * based on request templates.
 */
int main(int argc, char** argv){
  Options options = parseArguments(argc, argv);

  std::vector<std::string> allFilenames;
  if (options.protoFiles.empty()) {
    // If the user did not specify proto files, then parse all file names from
    // the filesystem, since SourceTreeDescriptorDatabase does not appear to
    // implement FindAllFileNames.
    for (const char* importPath: options.protoPaths) {
      std::filesystem::path protoDirPath(importPath);
      std::filesystem::recursive_directory_iterator it(protoDirPath,
          std::filesystem::directory_options::follow_directory_symlink);
      for (const std::filesystem::directory_entry& dir_entry : it) {
        if (dir_entry.is_regular_file()) {
          auto path = dir_entry.path();
          if (path.extension().string() == ".proto") {
            // We need to strip out the first directory because the
            // recursive_directory_iterator includes the name of the import path
            // and the proto tools assume paths relative to the import path.
            allFilenames.push_back(path.lexically_relative(protoDirPath));
          }
        }
      }
    }
  } else {
    // The user specified proto files so just use those. Note that we assume
    // the user has given filenames relative to at least one of the import
    // paths, just like protoc.
    for (const char* filename : options.protoFiles) {
      allFilenames.push_back(filename);
    }
  }

  if (options.verbose) {
    std::cerr << "Filenames that will be imported." << std::endl;
    for (std::string& filename: allFilenames) {
      std::cerr << '\t' << filename << std::endl;
    }
  }

  // Construct source tree and importer.
  DiskSourceTree diskSourceTree;
  for (const char* importPath: options.protoPaths) {
    diskSourceTree.MapPath("", importPath);
  }

  class ErrorReporter: public MultiFileErrorCollector {
    public:
      virtual void AddError(const std::string & filename, int line, int column, const std::string & message) {
        std::cerr << "Error occured for " << filename << ":" << line << ":" <<
            column  << " " << message << std::endl;
      }
  } errorReporter;

  // Build all the file protos, map the full service and method names to
  // ServiceDescriptor and MethodDescriptor respectively, because they're all
  // cross-linked.
  Importer importer(&diskSourceTree, &errorReporter);
  std::map<std::string, const ServiceDescriptor*> serviceDescriptors;
  std::map<std::string, const MethodDescriptor*> methodDescriptors;
  for (std::string& filename: allFilenames) {
    const FileDescriptor* fd = importer.Import(filename);
    if (fd == NULL) {
      std::cerr << "Encoutered errors causing a full FD on import. Aborting..." << std::endl;
      exit(1);
    }

    for (int i = 0; i < fd->service_count(); i++) {
      const ServiceDescriptor* service = fd->service(i);
      serviceDescriptors[tolower(service->full_name())] = service;
      for (int j = 0; j < service->method_count(); j++) {
        const MethodDescriptor* method = service->method(j);
        methodDescriptors[tolower(method->full_name())] = method;
      }
    }
  }

  // Install signal handler so that we exit with code 0 on Control-C.
  signal(SIGINT, [](int sig_num) {endCDK(); exit(0); });
  runCursesInterface(options, methodDescriptors);
}
