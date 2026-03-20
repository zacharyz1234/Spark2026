#pragma once
#include <string>
#include <vector>

// Represents a single code issue found by Bedrock during analysis.
// Maps directly to one element in the "issues" array of the Bedrock JSON response.
struct Issue {
    int line;                // Line number where the issue was found (-1 if not applicable)
    std::string severity;    // How bad it is: critical | high | medium | low
    std::string category;    // What kind of issue: security | performance | bug | style | maintainability
    std::string description; // One sentence explaining the problem
    std::string suggestion;  // One sentence explaining how to fix it
};

// One file to be analyzed — sent by the frontend (or Lambda) in the POST request body.
struct FileJob {
    std::string filePath;    // Path of the file (e.g. "/src/main.cpp"), used for display
    std::string codeContent; // Raw file content to send to Bedrock
};

// The analysis result for one file, returned directly in the HTTP response.
struct FileResult {
    std::string filePath;
    std::string status;        // "complete" | "error"
    std::vector<Issue> issues; // All issues found in this file
    int severityScore;         // Weighted sum: critical=4, high=3, medium=2, low=1
};
