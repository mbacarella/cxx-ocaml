(* fun with only newtype params: the return constraint is kept (a ghost
   Pexp_constraint) and `-> function` stays a Pexp_function *)
type ('a, 'b) t = T : ('a, 'a) t
let f : 'a 'b. ('a -> int) -> ('b -> int) -> ('a, 'b) t -> int =
  fun (type a b) : ((a -> int) -> (b -> int) -> (a, b) t -> int) ->
  fun _ _ T -> 0
let g = fun (type a b) : ((a, b) t -> int) -> fun T -> 0
let h : 'a 'b. ('a, 'b) t -> int = fun (type a b) : ((a, b) t -> int) -> function T -> 0
