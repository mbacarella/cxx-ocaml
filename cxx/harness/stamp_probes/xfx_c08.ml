module type S = sig type v end
module M = struct type v end
module F (X : S) = struct
  type t = A of int
end
module N = F (M)
let y = N.A 1
