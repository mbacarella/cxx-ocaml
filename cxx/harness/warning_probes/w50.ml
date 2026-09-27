[@@@warning "+50"]
let x = (** doc *) 1
(** ambiguous: after x, before y *)
let y = 2

(** floating: ocaml.text *)

type t = A (** info *) | B
and u = int

(** text of an [and] item *)

and v = float
let rec f x = g x (** after f *)
and g x = x
(**/**)
let () = (** unattached in an expression *) print_int (f x + y)
[@@@foo] (** after a floating attribute *)
