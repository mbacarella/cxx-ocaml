{ open Parser }
rule token = parse
  | [' ' '\t'] { token lexbuf }
  | '\n' { EOL }
  | ['0'-'9']+ as n { INT (int_of_string n) }
  | '+' { PLUS } | '*' { TIMES } | '(' { LPAREN } | ')' { RPAREN }
  | eof { EOL }
