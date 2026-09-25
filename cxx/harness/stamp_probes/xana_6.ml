module F (X : sig type 'a t end) = struct type 'a u = 'a X.t end
module N = F (struct type 'a t end)
