module type S = sig type v end
module F (X : S) = struct
  type 'a t = A of 'a
  let f : int t -> int = fun (A y) -> y
  let g = f (A 1)
end
