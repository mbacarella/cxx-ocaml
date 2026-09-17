module type S = sig type v end
module F (X : S) = struct
  type 'a t = A of 'a
  let f () = let y : int t = A 1 in ignore y
end
