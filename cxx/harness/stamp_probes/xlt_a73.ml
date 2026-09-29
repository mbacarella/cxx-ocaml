module Id (X : sig type t end) = X
module A = struct type t end
module type T = sig type u = Id (A).t type w = Id (A).t end
module P : T = struct type u = A.t type w = A.t end
