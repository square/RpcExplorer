# HACKING

This document is not fully fleshed out, because hq6 is currently the only
developer on this project.

If you are seriously interested in enhancing RpcExplorer, please file an issue.

## Dependencies
 * [Protocol Buffers](https://github.com/protocolbuffers/protobuf/blob/master/src/README.md)
 * [ncurses](https://ftp.gnu.org/pub/gnu/ncurses/)
 * [curses development kit](https://invisible-island.net/cdk/)

## What is a request template?

RpcExplorer is a tool that helps build proto requests for an Rpc method.

For reasons of flexibility, the core logic is decoupled from the details of
actually issuing requests, such as what tool to use and what port to hit, and
whether the output needs to be postprocessed.

All of those request details are captured in a request template file: a file
that looks like shell script, but includes special placeholder variables.
Request templates can contains two types of placeholders for substition:

1. Placeholders directly related to the service or proto that will be injected
   by the script when they are specified, based on normal user interaction.
    ```txt
    ###{JSON_REQUEST}
    ###{BASE64_PROTO_REQUEST}
    ###{PROTO_DIRS}
    ###{SERVICE_PROTO_FILE}
    ###{FULL_SERVICE_NAME}
    ###{FULL_METHOD_NAME}
    ###{SERVICE_NAME}
    ###{METHOD_NAME}
    ```
2. Placeholders to directly ask the user for. These can be any alphanumeric
   string and can contain spaces. The name will be displayed to the user. For
   example, the variable `###{registry name}` will turn into a question for the
   user at the time of request: "Please enter the registry name."
