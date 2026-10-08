module M = struct
  let ( .%{} ) a b = a + b
  let ( .%{}<- ) a b c = a + b + c
  let ( .%() ) a b = a * b
  let ( .%[] ) a b = a - b
  let ( .%{;..} ) a b = a + Array.length b
end
module N : sig
  val ( .%{} ) : int -> int -> int
  val ( .%{}<- ) : int -> int -> int -> int
  val ( .%() ) : int -> int -> int
  val ( .%[] ) : int -> int -> int
  val ( .%{;..} ) : int -> int array -> int
end = M
let f = M.( .%{} )
let g = N.( .%() )
let h x = x.M.%{3}
