%token <int> INT
%token PLUS TIMES LPAREN RPAREN EOL
%left PLUS
%left TIMES
%start main
%type <int> main
%%
main: expr EOL { $1 };
expr: INT { $1 } | LPAREN expr RPAREN { $2 } | expr PLUS expr { $1 + $3 } | expr TIMES expr { $1 * $3 };
