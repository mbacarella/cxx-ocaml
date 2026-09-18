module F (X : sig type t val compare : t -> t -> int end) =
  Set.Make (X)
module N = F (Int)
