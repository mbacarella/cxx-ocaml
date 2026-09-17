module type S = sig type v end
module F (X : S) = struct
  type 'a t = A of 'a
  module N = struct let x : int t = A 1 end
end
