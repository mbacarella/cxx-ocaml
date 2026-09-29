module F (X : sig type t end) = struct type u = X.t end
module N = F (struct type b = int type t = [ `A of 'a | `B of b ] as 'a end)
