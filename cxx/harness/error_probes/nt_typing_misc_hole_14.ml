

(* "_" parses as an expression (a "hole", Pexp_hole) anywhere a simple
   expression is allowed, but is rejected by the type-checker: holes
   are intended to be eliminated by a ppx rewriter. *)

type r = { a : int }
let f (x : int) = x
let fx ~x = x
let g ~_:x = x
let o ?_:(x = 0) () = x


(* "~_:" and "?_:" lex as labels named "_", so these do not involve
   holes at all. *)

let ok = g ~_:3


let ok = o ?_:(Some 5) ()


let a2 = Some _
