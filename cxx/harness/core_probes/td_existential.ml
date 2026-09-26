type any = Any : 'a * ('a -> string) -> any
let show (Any (x, f)) = f x
