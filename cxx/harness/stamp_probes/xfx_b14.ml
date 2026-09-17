module type S = sig type v end
module F (X : S) = struct
  type 'a t = 'a list
  let x : int t = []
end
