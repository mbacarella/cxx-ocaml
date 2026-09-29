module H : sig type t = int val equal : t -> t -> bool
  val seeded_hash : int -> t -> int end =
struct type t = int let equal (a : t) b = a = b
  let seeded_hash s (x : t) = s + x end
module S : Hashtbl.SeededS = Hashtbl.MakeSeeded(H)
