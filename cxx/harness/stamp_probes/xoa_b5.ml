module F (X : sig type t end) : sig type u val f : u -> u end = struct
  type u = X.t list
  let f (x : u) = x
end
module P = struct type t = int end
open F(P)
