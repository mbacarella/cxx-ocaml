(* Oracle token dumper: drives the reference OCaml lexer (compiler-libs) and
   prints the canonical token stream that cxx/src/token.cpp::print_canonical
   also produces, so the two can be diffed byte-for-byte.

   Build against the trunk compiler-libs (the stated oracle); can also be built
   against a stock compiler-libs for quick iteration. See build.sh. *)

let esc s =
  let b = Buffer.create (String.length s) in
  String.iter
    (fun ch ->
      let c = Char.code ch in
      match ch with
      | '\\' -> Buffer.add_string b "\\\\"
      | '"' -> Buffer.add_string b "\\\""
      | '\n' -> Buffer.add_string b "\\n"
      | '\r' -> Buffer.add_string b "\\r"
      | '\t' -> Buffer.add_string b "\\t"
      | _ ->
          if c >= 0x20 && c <= 0x7e then Buffer.add_char b ch
          else Buffer.add_string b (Printf.sprintf "\\x%02x" c))
    s;
  Buffer.contents b

let delim_repr = function None -> "-" | Some d -> "{" ^ d ^ "}"

(* Map a token to its canonical leading text (name + payload fields). *)
let canonical (tok : Parser.token) : string =
  let open Parser in
  match tok with
  (* payload-carrying *)
  | LIDENT s -> "LIDENT " ^ esc s
  | UIDENT s -> "UIDENT " ^ esc s
  | LABEL s -> "LABEL " ^ esc s
  | OPTLABEL s -> "OPTLABEL " ^ esc s
  | INFIXOP0 s -> "INFIXOP0 " ^ esc s
  | INFIXOP1 s -> "INFIXOP1 " ^ esc s
  | INFIXOP2 s -> "INFIXOP2 " ^ esc s
  | INFIXOP3 s -> "INFIXOP3 " ^ esc s
  | INFIXOP4 s -> "INFIXOP4 " ^ esc s
  | PREFIXOP s -> "PREFIXOP " ^ esc s
  | HASHOP s -> "HASHOP " ^ esc s
  | DOTOP s -> "DOTOP " ^ esc s
  | LETOP s -> "LETOP " ^ esc s
  | ANDOP s -> "ANDOP " ^ esc s
  | INT (s, m) -> (
      match m with None -> "INT " ^ esc s | Some c -> Printf.sprintf "INT %s %c" (esc s) c)
  | FLOAT (s, m) -> (
      match m with None -> "FLOAT " ^ esc s | Some c -> Printf.sprintf "FLOAT %s %c" (esc s) c)
  | CHAR c -> Printf.sprintf "CHAR %d" (Char.code c)
  | STRING (s, _loc, delim) -> Printf.sprintf "STRING %s %s" (delim_repr delim) (esc s)
  | QUOTED_STRING_EXPR (id, _, s, _, delim) ->
      Printf.sprintf "QUOTED_STRING_EXPR %s %s %s" (esc id) (delim_repr delim) (esc s)
  | QUOTED_STRING_ITEM (id, _, s, _, delim) ->
      Printf.sprintf "QUOTED_STRING_ITEM %s %s %s" (esc id) (delim_repr delim) (esc s)
  (* nullary: the constructor name is the canonical name *)
  | AMPERAMPER -> "AMPERAMPER" | AMPERSAND -> "AMPERSAND" | AND -> "AND"
  | AS -> "AS" | ASSERT -> "ASSERT" | BACKQUOTE -> "BACKQUOTE" | BANG -> "BANG"
  | BAR -> "BAR" | BARBAR -> "BARBAR" | BARRBRACKET -> "BARRBRACKET"
  | BEGIN -> "BEGIN" | CLASS -> "CLASS" | COLON -> "COLON"
  | COLONCOLON -> "COLONCOLON" | COLONEQUAL -> "COLONEQUAL"
  | COLONGREATER -> "COLONGREATER" | COMMA -> "COMMA" | CONSTRAINT -> "CONSTRAINT"
  | DO -> "DO" | DONE -> "DONE" | DOT -> "DOT" | DOTDOT -> "DOTDOT"
  | DOWNTO -> "DOWNTO" | EFFECT -> "EFFECT" | ELSE -> "ELSE" | END -> "END"
  | EOF -> "EOF" | EQUAL -> "EQUAL" | EXCEPTION -> "EXCEPTION"
  | EXTERNAL -> "EXTERNAL" | FALSE -> "FALSE" | FOR -> "FOR" | FUN -> "FUN"
  | FUNCTION -> "FUNCTION" | FUNCTOR -> "FUNCTOR" | GREATER -> "GREATER"
  | GREATERRBRACE -> "GREATERRBRACE" | GREATERRBRACKET -> "GREATERRBRACKET"
  | HASH -> "HASH" | IF -> "IF" | IN -> "IN" | INCLUDE -> "INCLUDE"
  | INHERIT -> "INHERIT" | INITIALIZER -> "INITIALIZER" | LAZY -> "LAZY"
  | LBRACE -> "LBRACE" | LBRACELESS -> "LBRACELESS" | LBRACKET -> "LBRACKET"
  | LBRACKETAT -> "LBRACKETAT" | LBRACKETATAT -> "LBRACKETATAT"
  | LBRACKETATATAT -> "LBRACKETATATAT" | LBRACKETBAR -> "LBRACKETBAR"
  | LBRACKETGREATER -> "LBRACKETGREATER" | LBRACKETLESS -> "LBRACKETLESS"
  | LBRACKETPERCENT -> "LBRACKETPERCENT"
  | LBRACKETPERCENTPERCENT -> "LBRACKETPERCENTPERCENT" | LESS -> "LESS"
  | LESSMINUS -> "LESSMINUS" | LET -> "LET" | LPAREN -> "LPAREN"
  | MATCH -> "MATCH" | METHOD -> "METHOD" | MINUS -> "MINUS"
  | MINUSDOT -> "MINUSDOT" | MINUSGREATER -> "MINUSGREATER" | MODULE -> "MODULE"
  | MUTABLE -> "MUTABLE" | NEW -> "NEW" | NONREC -> "NONREC" | OBJECT -> "OBJECT"
  | OF -> "OF" | OPEN -> "OPEN" | OR -> "OR" | PERCENT -> "PERCENT"
  | PLUS -> "PLUS" | PLUSDOT -> "PLUSDOT" | PLUSEQ -> "PLUSEQ"
  | PRIVATE -> "PRIVATE" | QUESTION -> "QUESTION" | QUOTE -> "QUOTE"
  | RBRACE -> "RBRACE" | RBRACKET -> "RBRACKET" | REC -> "REC" | RPAREN -> "RPAREN"
  | SEMI -> "SEMI" | SEMISEMI -> "SEMISEMI" | SIG -> "SIG" | STAR -> "STAR"
  | STRUCT -> "STRUCT" | THEN -> "THEN" | TILDE -> "TILDE" | TO -> "TO"
  | TRUE -> "TRUE" | TRY -> "TRY" | TYPE -> "TYPE" | UNDERSCORE -> "UNDERSCORE"
  | VAL -> "VAL" | VIRTUAL -> "VIRTUAL" | WHEN -> "WHEN" | WHILE -> "WHILE"
  | WITH -> "WITH"
  (* tokens not produced by the base lexer; present for exhaustiveness *)
  | tok -> ignore tok; "UNKNOWN"

let () =
  let file = Sys.argv.(1) in
  let ic = open_in_bin file in
  let n = in_channel_length ic in
  let s = really_input_string ic n in
  close_in ic;
  let lb = Lexing.from_string s in
  Location.input_name := file;
  Location.init lb file;
  Lexer.init ();
  let buf = Buffer.create 65536 in
  let rec loop () =
    match Lexer.token lb with
    | exception _ ->
        Buffer.add_string buf (Printf.sprintf "ERROR\t%d\n" (Lexing.lexeme_start lb))
    | tok ->
        let a = Lexing.lexeme_start lb and b = Lexing.lexeme_end lb in
        Buffer.add_string buf (Printf.sprintf "%s\t%d\t%d\n" (canonical tok) a b);
        if tok <> Parser.EOF then loop ()
  in
  loop ();
  print_string (Buffer.contents buf)
