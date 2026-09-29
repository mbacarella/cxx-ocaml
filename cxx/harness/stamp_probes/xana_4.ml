module F (X : sig type t end) (Y : sig type t end) = struct type u = X.t * Y.t end
module A = struct type t end
module N = F (A) (struct type t end)
