module Id (X : sig type t end) = X
module A = struct type t end
let f : Id (A).t -> int = fun _ -> 0
