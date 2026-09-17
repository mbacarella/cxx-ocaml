module Id (X : sig type t end) = X
module A = struct type t end
type u = [ `A of Id (A).t ]
