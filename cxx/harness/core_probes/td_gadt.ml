type _ t = Int : int t | Bool : bool t | Pair : 'a t * 'b t -> ('a * 'b) t
let rec eval : type a. a t -> string = function Int -> "i" | Bool -> "b" | Pair (x, y) -> eval x ^ eval y
