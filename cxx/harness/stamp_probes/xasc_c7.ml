module M : sig type t = int val compare : t -> t -> int end =
struct type t = int let compare = compare end
module S : sig
  type key
  type 'a t
  val empty : 'a t
end = Map.Make(M)
