module type S = sig type v end
module F (X : S) = struct
  type 'a t = A of 'a
  let f y = A y
  let g = f
end
