type _ c1 = A : int c1 | B : int c1 | C : char c1
type _ c2 = X : int c2 | Y : char c2 | Z : char c2
type _ repr = R1 : 'a c1 repr | R2 : 'a c2 repr
let f (type a b) (x : a repr) (y : b repr) = match x, y with R1, R2 -> 0
  | _, R1 -> 1
