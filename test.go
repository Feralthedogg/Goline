package test

import (
	"fmt"
)

func main() {
	var num1, num2 float64
	var operator string

	// Prompt user for the first number
	fmt.Print("Enter the first number: ")
	fmt.Scanln(&num1)

	// Prompt user for the operator
	fmt.Print("Enter an operator (+, -, *, /): ")
	fmt.Scanln(&operator)

	// Prompt user for the second number
	fmt.Print("Enter the second number: ")
	fmt.Scanln(&num2)

	// Perform the calculation based on the operator input
	switch operator {
	case "+":
		fmt.Printf("%.2f + %.2f = %.2f\n", num1, num2, num1+num2)
	case "-":
		fmt.Printf("%.2f - %.2f = %.2f\n", num1, num2, num1-num2)
	case "*":
		fmt.Printf("%.2f * %.2f = %.2f\n", num1, num2, num1*num2)
	case "/":
		if num2 == 0 {
			// Check for division by zero
			fmt.Println("Cannot divide by zero.")
		} else {
			fmt.Printf("%.2f / %.2f = %.2f\n", num1, num2, num1/num2)
		}
	default:
		// Handle unsupported operator input
		fmt.Println("Unsupported operator.")
	}
}
