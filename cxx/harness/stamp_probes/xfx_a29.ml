module type S = sig type v end
module F (X : S) = struct
  type 'a t = A of 'a
  exception E of int t
end
