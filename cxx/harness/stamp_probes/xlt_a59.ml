module Id (X : sig type t end) = X
module A = struct type t end
module P : sig type u = A.t val f : Id (A).t end = struct type u = Id (A).t
  let f : Id (A).t = assert false end
