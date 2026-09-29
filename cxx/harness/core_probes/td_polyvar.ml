type t = [ `A | `B of int ]
type u = [ t | `C ]
type 'a v = [< `A | `B > `A ] as 'a
