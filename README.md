# Rpc Explorer

This CLI tool allows a user to search for service methods defined in protobuf
and interactively build requests for those methods.
After building a request, a user can do one of the following.
1. Make a request and immediately view the response.
2. Export a bash script that makes the same request.

## Installation
### Install dependencies with homebrew
```sh
brew install grpcurl
brew install curl
```

### Build from source and add to PATH.

This will take 15-20 minutes because it builds all the dependencies from
source to maximize compatibility.
```sh
make
# Add the directory of RpcExplorer to your PATH variable.
```

## Usage
RpcExplorer is functionally an interactive script generator that relies on `grpcurl` to actually make requests.

To use RpcExplorer, you need the following:
 * One or more directories containing protobuf files that define services
   and the request and response protos for those services.
 * A request template, which looks like a bash script with special placeholders
   that are filled in by RpcExplorer.

Then, you pass these two pieces of information to RpcExplorer via command
line arguments.
```sh
RpcExplorer --request_template <path_to_request_template> \
  -I <proto_dir_1> \
  -I <proto_dir_2> \
  ...
```
Note: If you are running on OSX, you must set TERM=nsterm for Backspace to
work correctly. Otherwise you will need to use Control-H for backspace.

## Sample invocations

To try the following example, you can build and run the [hello world](https://github.com/grpc/grpc/blob/master/examples/protos/helloworld.proto)
server from the [gRPC repo](https://github.com/grpc/grpc/) in your favorite language.
```sh
# Invoke this to see all the variables that RpcExplorer can export.
./RpcExplorer -I example/protos \
  --request_template templates/echo_all_variables.sh.template

# Invoke this to use grpcurl to talk to a plaintext service of your choice.
# You can copy and modify this template for SSL.
./RpcExplorer -I example/protos \
  --request_template templates/grpcurl_plaintext.sh.template
```

## Guide to the curses interface.

 * TAB is used to navigate between panes or entries on the same page.
 * ENTER is used to dismiss information windows and to confirm selections.
 * On most screens, F8 will reset to the first screen.

### Search Page
 * Type your search string and type ENTER or TAB to show results and switch to
   the next window.
 * Use UP and DOWN arrow keys to select a method, and ENTER to make a choice.
 * Use TAB to go back to the search.

### Request Builder Page
 * Use TAB to move to the next entry.
 * Use SHIFT-TAB to move to the previous entry.
 * Use F1 on a Message or Enum field to view the proto definition.
 * Use F1 outside a Message or Enum field to view the proto definition of the enclosing message.
 * Use the "Make Request" button to make an interactive request.
 * Use the "Export Script" button to export the CLI script.

If you want to help improve RpcExplorer, please see the [HACKING](HACKING.md)
document and reach out to hq6 by filing an issue.

