module type S = sig type v end
module F (X : S) = struct
  type 'a t = private 'a list
  let f : int t -> int = fun _ -> 0
end
