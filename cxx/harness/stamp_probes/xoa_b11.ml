module F (X : sig type t end) (Y : sig type s end) = struct
  type u = X.t * Y.s
  let f (x : u) = x
end
module P = struct type t = int end
module Q = struct type s = int end
open F(P)(Q)
