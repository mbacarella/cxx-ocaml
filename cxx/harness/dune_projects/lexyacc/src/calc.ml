let () = print_int (Parser.main Lexer.token (Lexing.from_string "1+2*(3+4)\n")); print_newline ()
