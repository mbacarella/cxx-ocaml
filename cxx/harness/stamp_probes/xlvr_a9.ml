module F (X : sig end) = struct type 'a t = A of 'a end
module G = F (struct end)
type 'a u = 'a G.t
