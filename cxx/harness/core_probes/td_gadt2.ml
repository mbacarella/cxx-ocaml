type 'a expr = Lit : int -> int expr | Add : int expr * int expr -> int expr | If : bool expr * 'a expr * 'a expr -> 'a expr | Eq : int expr * int expr -> bool expr
