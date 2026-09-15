module H : sig type t = int val equal : t -> t -> bool
  val hash : t -> int end =
struct type t = int let equal (a : t) b = a = b let hash (x : t) = x end
module S : Weak.S = Weak.Make(H)
