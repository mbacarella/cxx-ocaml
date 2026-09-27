type _ expr =
  | Int : int -> int expr
  | Bool : bool -> bool expr
  | Add : int expr * int expr -> int expr
  | If : bool expr * 'a expr * 'a expr -> 'a expr
  | Pair : 'a expr * 'b expr -> ('a * 'b) expr

let rec eval : type a. a expr -> a = function
  | Int n -> n
  | Bool b -> b
  | Add (a, b) -> eval a + eval b
  | If (c, t, e) -> if eval c then eval t else eval e
  | Pair (a, b) -> (eval a, eval b)

type any = Any : 'a expr -> any
let size (Any e) = let rec go : type a. a expr -> int = function
  | Int _ | Bool _ -> 1 | Add (a, b) -> 1 + go a + go b
  | If (c, t, e) -> 1 + go c + go t + go e | Pair (a, b) -> 1 + go a + go b in go e
