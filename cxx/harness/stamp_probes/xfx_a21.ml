module type S = sig type v end
module F (X : S) = struct
  type 'a t = A of 'a
  type u = int t
  type w = string t
end
