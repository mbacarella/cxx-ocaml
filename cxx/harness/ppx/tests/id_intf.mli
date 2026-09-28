(** docs *)
type t [@@immediate]
val v : t
val f : ?x:int -> (int -> int) -> int [@@noalloc] [@@ocaml.deprecated]
module type M = sig val x : int end
module N : M
