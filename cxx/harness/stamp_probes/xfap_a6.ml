module F (X : sig type t end) (Y : sig type t end) =
  struct type t = X.t * Y.t end
module P (B : sig type t end) = F (B) (B)
