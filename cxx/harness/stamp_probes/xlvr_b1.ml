module F (X : sig end) = struct type 'a t = A of ('a -> unit) end
module Y = struct end
type 'a u = 'a F(Y).t
