module type S = sig type v end
module F (X : S) = struct
  type t = A of int
  let x : t = A 1
end
