type _ c1 = A : int c1 | B : int c1 | C : char c1
type _ c2 = X : int c2 | Y : char c2 | Z : char c2
type _ repr = R1 : 'a c1 repr | R2 : 'a c2 repr
let f (type a) (x : a repr) (y : a) = match x, y with R1, _ -> 0 | R2, _ -> 1
