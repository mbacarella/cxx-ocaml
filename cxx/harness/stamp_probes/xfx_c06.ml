module type S = sig type v end
module M = struct type v end
module F (X : S) = struct
  type 'a t = A of 'a
end
module N = F (M)
let y = N.A 1
