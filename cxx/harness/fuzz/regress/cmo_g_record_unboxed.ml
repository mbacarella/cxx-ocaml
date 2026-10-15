(* FUZZ: levels=bcg
   Found by camlsmith port mode (night1 campaign, seed 3): with -g, a debug
   event's environment holds the declarations; typedecl.ml's constant
   `Record_unboxed false` and its `Type_abstract Definition` are the same
   block {tag 0; 0} in ocamlopt (one structured constant), so an [@@unboxed]
   record's representation shares an abstract type's kind in the .cmo. *)
type t
type pa = [ `X of int | `Y ]
type ub = { ua : int } [@@unboxed]
let f (x : ub) (y : pa) = match y with `X n -> x.ua + n | `Y -> x.ua
let () = print_int (f { ua = 3 } `Y)
