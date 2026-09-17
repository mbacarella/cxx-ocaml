module Id (X : sig type t end) = X
module A = struct type t end
module P = (struct type u = Id (A).t end : sig type u end)
