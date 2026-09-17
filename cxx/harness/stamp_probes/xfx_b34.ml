module type S = sig type v end
module F (X : S) = struct
  type 'a t = A of 'a
  type 'a u = B of 'a
  let x = A 1
  let y = B 1
end
