type t = ..
type t += I of { z : int } | H | K of { w : int } | L
type m1 = int
type t += private P
type m2 = int
type 'a u = ..
type 'a u += Q : 'a -> 'a u | R of 'a
type m3 = int
