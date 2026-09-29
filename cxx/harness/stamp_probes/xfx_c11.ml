module type S = sig type v end
module M = struct type v end
module F (X : S) = struct
  type 'a t = A of 'a
  let x : int t = A 1
end
module N = F (M)
let f () = N.x
