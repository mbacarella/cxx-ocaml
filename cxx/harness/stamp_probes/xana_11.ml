module F (X : sig type t end) = struct type u = X.t end
module N = F (struct type t = [ `A of 'a | `B ] as 'a end)
