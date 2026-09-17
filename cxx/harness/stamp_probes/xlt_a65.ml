module P = struct module Id (X : sig type t end) = X end
module A = struct type t end
type u = P.Id (A).t
