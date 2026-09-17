module type S = sig type v end
module F (X : S) = struct
  module N = struct type 'a t = A of 'a end
  let x : int N.t = N.A 1
end
