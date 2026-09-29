module type S = sig type v end
module F (X : S) = struct
  type 'a t = A of 'a
  let f (x : int t) = ()
end
