(* another unit's abstract type, through a tuple: Deepsep *)
type 'a t = ('a * int, int) Hashtbl.t
